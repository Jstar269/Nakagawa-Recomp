/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Add to Steam (#2152, src/core/nk_steam_shortcuts.c): the binary shortcuts.vdf edit and the
 * file-level rules around it. The files are built here byte by byte in a temporary folder; no
 * Steam client, registry entry or network is involved. */

#include "nk_platform.h"
#include "nk_steam_shortcuts.h"
#include "native_test_isolation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fixture paths are short temporary-folder names; the compiler can only assume the longest
 * a buffer allows, and every path is checked by the calls that use it. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));     \
        }                                                                        \
    } while (0)

#define CHECK_STATUS(actual, expected, what)                                     \
    do {                                                                         \
        ++g_checks;                                                              \
        if ((actual) != (expected)) {                                            \
            ++g_failures;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: got %s, want %s\n", __FILE__,       \
                    __LINE__, (what), nk_steam_status_name(actual),              \
                    nk_steam_status_name(expected));                             \
        }                                                                        \
    } while (0)

static const NkSteamShortcut kPlayer = {
    "Nakagawa Recomp", "C:/Games/Nakagawa/bin/nakagawa_player.exe", "C:/Games/Nakagawa/bin", ""};

/* ---- A byte builder for existing files ---- */

typedef struct {
    uint8_t data[8192];
    size_t size;
} Raw;

static void raw_byte(Raw *r, uint8_t b) { r->data[r->size++] = b; }
static void raw_cstr(Raw *r, const char *s) {
    memcpy(r->data + r->size, s, strlen(s) + 1u);
    r->size += strlen(s) + 1u;
}
static void raw_str(Raw *r, const char *key, const char *value) {
    raw_byte(r, 0x01);
    raw_cstr(r, key);
    raw_cstr(r, value);
}
static void raw_int(Raw *r, const char *key, uint32_t value) {
    raw_byte(r, 0x02);
    raw_cstr(r, key);
    for (int i = 0; i < 4; i++) raw_byte(r, (uint8_t)(value >> (8 * i)));
}
/* One entry as Steam itself wrote it, with a lower-case "exe" key and fields this code
 * never writes (a tag and a float), so the preserved bytes are not only our own layout. */
static void raw_entry(Raw *r, const char *index, const char *name, const char *quoted_exe) {
    raw_byte(r, 0x00);
    raw_cstr(r, index);
    raw_int(r, "appid", 0x80001234u);
    raw_str(r, "AppName", name);
    raw_str(r, "exe", quoted_exe);
    raw_str(r, "StartDir", "\"C:/Other\"");
    raw_int(r, "LastPlayTime", 1700000000u);
    raw_byte(r, 0x03);
    raw_cstr(r, "Score");
    for (int i = 0; i < 4; i++) raw_byte(r, 0);
    raw_byte(r, 0x00);
    raw_cstr(r, "tags");
    raw_str(r, "0", "favorite");
    raw_byte(r, 0x08);
    raw_byte(r, 0x08);
}
static void raw_open(Raw *r) {
    raw_byte(r, 0x00);
    raw_cstr(r, "shortcuts");
}
static void raw_close(Raw *r) {
    raw_byte(r, 0x08);
    raw_byte(r, 0x08);
}

static void two_entries(Raw *r) {
    memset(r, 0, sizeof(*r));
    raw_open(r);
    raw_entry(r, "0", "Some Game", "\"C:/Games/A/a.exe\"");
    raw_entry(r, "1", "Other Game", "\"C:/Games/B/b.exe\"");
    raw_close(r);
}

/* ---- Files ---- */

static char g_root[NATIVE_TEST_PATH_MAX];

static void path_in(char *out, size_t cap, const char *leaf) {
    snprintf(out, cap, "%s/%s", g_root, leaf);
}

static bool write_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) return false;
    bool ok = size == 0u || fwrite(data, 1u, size, f) == size;
    return fclose(f) == 0 && ok;
}

static uint8_t *read_bytes(const char *path, size_t *size) {
    *size = 0;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return NULL;
    uint8_t *data = (uint8_t *)malloc(1u << 20);
    if (!data) {
        fclose(f);
        return NULL;
    }
    *size = fread(data, 1u, 1u << 20, f);
    fclose(f);
    return data;
}

static bool file_is(const char *path, const uint8_t *data, size_t size) {
    size_t got = 0;
    uint8_t *back = read_bytes(path, &got);
    bool same = back && got == size && (size == 0u || memcmp(back, data, size) == 0);
    free(back);
    return same;
}

static const void *find_bytes(const void *hay, size_t hay_size, const void *needle, size_t needle_size) {
    const uint8_t *h = (const uint8_t *)hay;
    for (size_t i = 0; needle_size <= hay_size && i + needle_size <= hay_size; i++) {
        if (memcmp(h + i, needle, needle_size) == 0) return h + i;
    }
    return NULL;
}

static bool is_running_yes(void) { return true; }
static bool is_running_no(void) { return false; }

/* ---- Tests ---- */

static void test_app_id(void) {
    /* The value zlib's crc32 gives for the quoted path followed by the name, top bit set. */
    CHECK(nk_steam_shortcut_app_id(kPlayer.exe, kPlayer.app_name) == 0xcb3fa139u,
          "the app ID is the CRC-32 of the quoted exe and name, top bit set");
    CHECK((nk_steam_shortcut_app_id("a", "b") & 0x80000000u) != 0u, "the top bit is always set");
}

static void test_new_file(void) {
    uint8_t *out = NULL;
    size_t size = 0;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &kPlayer, &out, &size, NULL), NK_STEAM_OK, "new file");
    CHECK(out && size > 16u, "a new file has content");
    if (!out) return;
    static const uint8_t head[] = {0x00, 's', 'h', 'o', 'r', 't', 'c', 'u', 't', 's', 0x00,
                                   0x00, '0', 0x00};
    CHECK(size >= sizeof(head) && memcmp(out, head, sizeof(head)) == 0,
          "starts with the shortcuts map and entry 0");
    CHECK(size >= 2u && out[size - 1u] == 0x08 && out[size - 2u] == 0x08,
          "ends with the end of shortcuts and the end of the root");
    bool found = false;
    CHECK_STATUS(nk_steam_vdf_has_exe(out, size, kPlayer.exe, &found), NK_STEAM_OK, "re-parse");
    CHECK(found, "the new file lists the executable");
    /* The executable is stored quoted, as Steam does, and the app ID is written. */
    static const char quoted[] = "\"C:/Games/Nakagawa/bin/nakagawa_player.exe\"";
    CHECK(find_bytes(out, size, quoted, sizeof(quoted)) != NULL, "the executable is quoted");
    static const uint8_t appid[] = {0x02, 'a', 'p', 'p', 'i', 'd', 0x00, 0x39, 0xa1, 0x3f, 0xcb};
    CHECK(find_bytes(out, size, appid, sizeof(appid)) != NULL, "the app ID is written little endian");
    nk_steam_free(out);
}

static void test_existing_entries_are_preserved(void) {
    Raw in;
    two_entries(&in);
    uint8_t *out = NULL;
    size_t size = 0;
    CHECK_STATUS(nk_steam_vdf_add(in.data, in.size, &kPlayer, &out, &size, NULL), NK_STEAM_OK,
                 "add to a file with two entries");
    if (!out) return;
    size_t end = in.size - 2u; /* the two 0x08 */
    CHECK(size > in.size, "the file grew");
    CHECK(memcmp(out, in.data, end) == 0, "every earlier byte, entry 0 and entry 1, is unchanged");
    CHECK(memcmp(out + size - 2u, in.data + end, 2u) == 0, "the closing bytes are unchanged");
    CHECK(out[end] == 0x00 && out[end + 1u] == '2' && out[end + 2u] == 0x00,
          "the new entry is key 2, after the last one");
    bool found = false;
    nk_steam_vdf_has_exe(out, size, "C:/Games/B/b.exe", &found);
    CHECK(found, "the other games are still listed");
    nk_steam_free(out);
}

static void test_three_adds_number_in_order(void) {
    NkSteamShortcut a = {"A", "C:/a/a.exe", "C:/a", NULL};
    NkSteamShortcut b = {"B", "C:/b/b.exe", "C:/b", "--ui=next"};
    NkSteamShortcut c = {"C", "C:/c/c.exe", "C:/c", ""};
    uint8_t *one = NULL, *two = NULL, *three = NULL;
    size_t n1 = 0, n2 = 0, n3 = 0;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &a, &one, &n1, NULL), NK_STEAM_OK, "first");
    CHECK_STATUS(nk_steam_vdf_add(one, n1, &b, &two, &n2, NULL), NK_STEAM_OK, "second");
    CHECK_STATUS(nk_steam_vdf_add(two, n2, &c, &three, &n3, NULL), NK_STEAM_OK, "third");
    if (three) {
        static const uint8_t k1[] = {0x00, '1', 0x00, 0x02, 'a', 'p', 'p', 'i', 'd'};
        static const uint8_t k2[] = {0x00, '2', 0x00, 0x02, 'a', 'p', 'p', 'i', 'd'};
        CHECK(find_bytes(three, n3, k1, sizeof(k1)) != NULL, "entry 1 exists");
        CHECK(find_bytes(three, n3, k2, sizeof(k2)) != NULL, "entry 2 exists");
        CHECK(find_bytes(three, n3, "--ui=next", 10u) != NULL, "launch options are written");
    }
    nk_steam_free(one);
    nk_steam_free(two);
    nk_steam_free(three);
}

static void test_existing_executable_is_reported_not_added(void) {
    Raw in;
    two_entries(&in);
    uint8_t *out = (uint8_t *)1;
    size_t size = 7;
    NkSteamShortcut same = {"Renamed", "c:/games/a/A.EXE", "C:/Games/A", ""};
    CHECK_STATUS(nk_steam_vdf_add(in.data, in.size, &same, &out, &size, NULL), NK_STEAM_ERR_EXISTS,
                 "the same executable, in another case and with another name");
    CHECK(out == NULL && size == 0u, "nothing is returned");
}

static void test_no_shortcuts_map_gets_one(void) {
    static const uint8_t only_root_end[] = {0x08};
    uint8_t *out = NULL;
    size_t size = 0;
    CHECK_STATUS(nk_steam_vdf_add(only_root_end, 1u, &kPlayer, &out, &size, NULL), NK_STEAM_OK,
                 "a valid file without a shortcuts map");
    bool found = false;
    CHECK(out && nk_steam_vdf_has_exe(out, size, kPlayer.exe, &found) == NK_STEAM_OK && found,
          "the map is created and lists the executable");
    nk_steam_free(out);
}

static void test_corrupt_files_are_refused(void) {
    Raw good;
    two_entries(&good);
    uint8_t *out = NULL;
    size_t size = 0;

    /* Every proper prefix is damaged, never accepted, never a crash. */
    int accepted = 0;
    for (size_t cut = 1; cut < good.size; cut++) {
        NkSteamStatus s = nk_steam_vdf_add(good.data, cut, &kPlayer, &out, &size, NULL);
        if (s == NK_STEAM_OK) accepted++;
        CHECK(s == NK_STEAM_ERR_CORRUPT || s == NK_STEAM_OK, "a cut file is corrupt or still whole");
        nk_steam_free(out);
        out = NULL;
    }
    /* The one cut that stays valid is after the first root map closes: none here, since the
     * file ends with the only two closing bytes. */
    CHECK(accepted == 0, "no proper prefix of a file is accepted as a file");

    Raw bad;
    memcpy(&bad, &good, sizeof(bad));
    bad.data[bad.size++] = 0x00; /* trailing byte */
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size, NULL), NK_STEAM_ERR_CORRUPT,
                 "trailing bytes");

    memset(&bad, 0, sizeof(bad));
    raw_open(&bad);
    raw_byte(&bad, 0x09); /* an unknown type */
    raw_cstr(&bad, "x");
    raw_close(&bad);
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size, NULL), NK_STEAM_ERR_CORRUPT,
                 "an unknown value type");

    memset(&bad, 0, sizeof(bad));
    raw_open(&bad);
    raw_byte(&bad, 0x01);
    raw_cstr(&bad, "key");
    memcpy(bad.data + bad.size, "no terminator", 13u); /* a string that never ends */
    bad.size += 13u;
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size, NULL), NK_STEAM_ERR_CORRUPT,
                 "an unterminated string");

    memset(&bad, 0, sizeof(bad));
    for (int i = 0; i < 40; i++) {
        raw_byte(&bad, 0x00);
        raw_cstr(&bad, "n");
    }
    for (int i = 0; i < 41; i++) raw_byte(&bad, 0x08);
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size, NULL), NK_STEAM_ERR_CORRUPT,
                 "nesting past the cap");

    memset(&bad, 0, sizeof(bad));
    raw_open(&bad);
    raw_byte(&bad, 0x02);
    raw_cstr(&bad, "short");
    raw_byte(&bad, 1); /* an int32 with one byte */
    CHECK_STATUS(nk_steam_vdf_add(bad.data, bad.size, &kPlayer, &out, &size, NULL), NK_STEAM_ERR_CORRUPT,
                 "a short integer");

    CHECK_STATUS(nk_steam_vdf_add((const uint8_t *)"text", 4u, &kPlayer, &out, &size, NULL),
                 NK_STEAM_ERR_CORRUPT, "plain text");

    /* Single-byte damage must never crash. A refused file yields no output and leaves its input as it
       was; an accepted one parses, lists the executable, and keeps every byte of the damaged input
       but for the new entry (everything before the last two end markers, and those two). */
    for (size_t at = 0; at < good.size; at++) {
        Raw flip, before;
        memcpy(&flip, &good, sizeof(flip));
        flip.data[at] ^= 0xFFu;
        memcpy(&before, &flip, sizeof(before));
        out = (uint8_t *)0x1;
        size = 12345u;
        NkSteamStatus s = nk_steam_vdf_add(flip.data, flip.size, &kPlayer, &out, &size, NULL);
        CHECK(memcmp(&before, &flip, sizeof(flip)) == 0, "the input is not modified");
        if (s == NK_STEAM_OK) {
            bool found = false;
            CHECK(nk_steam_vdf_has_exe(out, size, kPlayer.exe, &found) == NK_STEAM_OK && found,
                  "a returned file parses and lists the executable");
            CHECK(size > flip.size && memcmp(out, flip.data, flip.size - 2u) == 0 &&
                      memcmp(out + size - 2u, flip.data + flip.size - 2u, 2u) == 0,
                  "an accepted file keeps every byte it had");
        } else {
            CHECK(out == NULL && size == 0u, "a refused file yields no output");
            out = NULL;
        }
        nk_steam_free(out);
        out = NULL;
    }
}

static void test_steam_running_fails_closed(void) {
    CHECK(!nk_steam_open_error_means_running(87ul), "a pid that is gone is not running");
    CHECK(nk_steam_open_error_means_running(5ul), "access denied (an elevated Steam) is running");
    CHECK(nk_steam_open_error_means_running(0ul), "an unexplained failure is running");
    CHECK(nk_steam_open_error_means_running(1450ul), "any other error is running");
    CHECK(!nk_steam_registry_error_means_running(2ul), "no pid value: Steam never ran");
    CHECK(!nk_steam_registry_error_means_running(3ul), "no pid key: Steam never ran");
    CHECK(nk_steam_registry_error_means_running(5ul), "a pid that cannot be read is running");
    CHECK(nk_steam_registry_error_means_running(234ul), "a short buffer is running");
    uint8_t *out = NULL;
    size_t size = 0;
    NkSteamShortcut quoted = kPlayer;
    quoted.exe = "\"C:/Games/Nakagawa/nakagawa_player.exe\"";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &quoted, &out, &size, NULL), NK_STEAM_ERR_ARGUMENT,
                 "a quoted executable is refused: the app ID adds the quotes itself");
}

static void test_arguments(void) {
    uint8_t *out = NULL;
    size_t size = 0;
    NkSteamShortcut bad = kPlayer;
    bad.app_name = "";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size, NULL), NK_STEAM_ERR_ARGUMENT, "empty name");
    bad = kPlayer;
    bad.exe = NULL;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size, NULL), NK_STEAM_ERR_ARGUMENT, "no exe");
    bad = kPlayer;
    bad.start_dir = "C:/x\ny";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size, NULL), NK_STEAM_ERR_ARGUMENT,
                 "a control character in a field");
    bad = kPlayer;
    bad.launch_options = "--a\tb";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size, NULL), NK_STEAM_ERR_ARGUMENT,
                 "a tab in the launch options");
    char huge[NK_STEAM_FIELD_MAX + 8u];
    memset(huge, 'x', sizeof(huge) - 1u);
    huge[sizeof(huge) - 1u] = '\0';
    bad = kPlayer;
    bad.app_name = huge;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size, NULL), NK_STEAM_ERR_ARGUMENT,
                 "a name past the cap");
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, NULL, &out, &size, NULL), NK_STEAM_ERR_ARGUMENT, "no shortcut");
    /* A non-ASCII name is fine: its bytes are all 0x80 or more. */
    bad = kPlayer;
    bad.app_name = "\xE4\xB8\xAD\xE5\xB7\x9D";
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &bad, &out, &size, NULL), NK_STEAM_OK, "a UTF-8 name");
    nk_steam_free(out);
}

static void test_file_add_and_backup(void) {
    char dir[NATIVE_TEST_PATH_MAX], vdf[NATIVE_TEST_PATH_MAX], backup[NK_MAX_PATH];
    path_in(dir, sizeof(dir), "file-add");
    snprintf(vdf, sizeof(vdf), "%s/config/shortcuts.vdf", dir);
    Raw in;
    two_entries(&in);
    CHECK(nk_platform_mkdir_p(dir), "make the folder");
    char config[NATIVE_TEST_PATH_MAX];
    snprintf(config, sizeof(config), "%s/config", dir);
    CHECK(nk_platform_mkdir_p(config), "make config");
    CHECK(write_bytes(vdf, in.data, in.size), "write the existing file");

    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, false, "20261106-120000", backup,
                                      sizeof(backup), NULL), NK_STEAM_OK, "add");
    CHECK(strstr(backup, "shortcuts.vdf.nakagawa-backup-20261106-120000") != NULL,
          "the backup is named for the stamp");
    CHECK(file_is(backup, in.data, in.size), "the backup holds the original bytes");
    size_t now = 0;
    uint8_t *after = read_bytes(vdf, &now);
    bool found = false;
    CHECK(after && nk_steam_vdf_has_exe(after, now, kPlayer.exe, &found) == NK_STEAM_OK && found,
          "the file lists the player");
    CHECK(after && memcmp(after, in.data, in.size - 2u) == 0, "earlier entries are intact");
    free(after);
    char temp[NATIVE_TEST_PATH_MAX];
    snprintf(temp, sizeof(temp), "%s.nakagawa-tmp", vdf);
    CHECK(nk_platform_path_missing(temp), "no temporary file is left");

    /* A second entry with the same stamp must not overwrite the first backup. */
    NkSteamShortcut second = {"Second", "C:/Second/s.exe", "C:/Second", ""};
    char backup2[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &second, false, "20261106-120000", backup2,
                                      sizeof(backup2), NULL), NK_STEAM_OK, "add again");
    CHECK(strcmp(backup, backup2) != 0, "a second backup has its own name");
    CHECK(file_is(backup, in.data, in.size), "the first backup is untouched");

    /* The same executable again is reported and writes nothing, no backup either. */
    size_t before_size = 0;
    uint8_t *before = read_bytes(vdf, &before_size);
    char backup3[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, false, "20261106-120001", backup3,
                                      sizeof(backup3), NULL), NK_STEAM_ERR_EXISTS, "existing entry");
    CHECK(backup3[0] == '\0', "no backup for a refused add");
    CHECK(before && file_is(vdf, before, before_size), "the file is unchanged");
    free(before);
}

static void test_running_steam_blocks_every_write(void) {
    char dir[NATIVE_TEST_PATH_MAX], vdf[NATIVE_TEST_PATH_MAX];
    path_in(dir, sizeof(dir), "running");
    snprintf(vdf, sizeof(vdf), "%s/shortcuts.vdf", dir);
    Raw in;
    two_entries(&in);
    CHECK(nk_platform_mkdir_p(dir) && write_bytes(vdf, in.data, in.size), "fixture");
    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, true, "20261106-120000", backup,
                                      sizeof(backup), NULL), NK_STEAM_ERR_STEAM_RUNNING, "Steam runs");
    CHECK(file_is(vdf, in.data, in.size), "the file is untouched");
    char leftover[NATIVE_TEST_PATH_MAX];
    snprintf(leftover, sizeof(leftover), "%s.nakagawa-backup-20261106-120000", vdf);
    CHECK(nk_platform_path_missing(leftover), "no backup is made either");
}

static void test_corrupt_file_is_left_alone(void) {
    char dir[NATIVE_TEST_PATH_MAX], vdf[NATIVE_TEST_PATH_MAX], leftover[NATIVE_TEST_PATH_MAX];
    path_in(dir, sizeof(dir), "corrupt");
    snprintf(vdf, sizeof(vdf), "%s/shortcuts.vdf", dir);
    static const uint8_t junk[] = "this is not a shortcuts file";
    CHECK(nk_platform_mkdir_p(dir) && write_bytes(vdf, junk, sizeof(junk)), "fixture");
    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, false, "20261106-120000", backup,
                                      sizeof(backup), NULL), NK_STEAM_ERR_CORRUPT, "corrupt file");
    CHECK(file_is(vdf, junk, sizeof(junk)), "the file is untouched");
    snprintf(leftover, sizeof(leftover), "%s.nakagawa-backup-20261106-120000", vdf);
    CHECK(nk_platform_path_missing(leftover), "no backup of a file that was refused");
    snprintf(leftover, sizeof(leftover), "%s.nakagawa-tmp", vdf);
    CHECK(nk_platform_path_missing(leftover), "no temporary file");
}

static void test_a_failed_backup_writes_nothing(void) {
    char dir[NATIVE_TEST_PATH_MAX], vdf[NATIVE_TEST_PATH_MAX], name[NATIVE_TEST_PATH_MAX];
    path_in(dir, sizeof(dir), "nobackup");
    snprintf(vdf, sizeof(vdf), "%s/shortcuts.vdf", dir);
    Raw in;
    two_entries(&in);
    CHECK(nk_platform_mkdir_p(dir) && write_bytes(vdf, in.data, in.size), "fixture");
    /* Every backup name the code would try already exists. */
    snprintf(name, sizeof(name), "%s.nakagawa-backup-20261106-120000", vdf);
    CHECK(write_bytes(name, in.data, 1u), "first backup name taken");
    for (unsigned i = 2; i <= 100u; i++) {
        snprintf(name, sizeof(name), "%s.nakagawa-backup-20261106-120000-%u", vdf, i);
        CHECK(write_bytes(name, in.data, 1u), "backup name taken");
    }
    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kPlayer, false, "20261106-120000", backup,
                                      sizeof(backup), NULL), NK_STEAM_ERR_BACKUP, "no backup possible");
    CHECK(file_is(vdf, in.data, in.size), "the original is untouched");
    snprintf(name, sizeof(name), "%s.nakagawa-backup-20261106-120000", vdf);
    CHECK(file_is(name, in.data, 1u), "an earlier backup is not overwritten");
    snprintf(name, sizeof(name), "%s.nakagawa-backup-20261106-120000-100", vdf);
    CHECK(file_is(name, in.data, 1u), "the last backup name tried is not deleted");
}

static void test_bad_stamp(void) {
    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_to_file("unused.vdf", &kPlayer, false, "../x", backup, sizeof(backup), NULL),
                 NK_STEAM_ERR_ARGUMENT, "a stamp with path characters");
    CHECK_STATUS(nk_steam_add_to_file("unused.vdf", &kPlayer, false, "", backup, sizeof(backup), NULL),
                 NK_STEAM_ERR_ARGUMENT, "an empty stamp");
}

static void test_accounts_and_add_for_account(void) {
    char steam[NATIVE_TEST_PATH_MAX], sub[NATIVE_TEST_PATH_MAX];
    path_in(steam, sizeof(steam), "Steam");
    const char *dirs[] = {"0", "30001", "4", "abc", "12x", "20002"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(sub, sizeof(sub), "%s/userdata/%s", steam, dirs[i]);
        CHECK(nk_platform_mkdir_p(sub), "make an account folder");
    }
    snprintf(sub, sizeof(sub), "%s/userdata/777", steam);
    CHECK(write_bytes(sub, (const uint8_t *)"f", 1u), "a plain file named like an account");

    NkSteamAccount accounts[NK_STEAM_ACCOUNT_CAP];
    size_t count = 0;
    CHECK_STATUS(nk_steam_list_accounts(steam, accounts, NK_STEAM_ACCOUNT_CAP, &count), NK_STEAM_OK,
                 "list accounts");
    CHECK(count == 3u, "only the digit folders other than 0 are accounts");
    if (count == 3u) {
        CHECK(strcmp(accounts[0].id, "4") == 0 && strcmp(accounts[1].id, "20002") == 0 &&
              strcmp(accounts[2].id, "30001") == 0, "ascending by number");
        char want[NK_MAX_PATH];
        char sep = nk_platform_path_separator();
        snprintf(want, sizeof(want), "userdata%c20002%cconfig%cshortcuts.vdf", sep, sep, sep);
        CHECK(strstr(accounts[1].shortcuts_path, want) != NULL,
              "the shortcuts path is under the account's config folder");
    }
    size_t one = 0;
    CHECK_STATUS(nk_steam_list_accounts(steam, accounts, 2u, &one), NK_STEAM_OK, "a small cap");
    CHECK(one <= 2u, "never more than the cap");

    char missing[NATIVE_TEST_PATH_MAX];
    path_in(missing, sizeof(missing), "no-steam-here");
    CHECK_STATUS(nk_steam_list_accounts(missing, accounts, NK_STEAM_ACCOUNT_CAP, &count),
                 NK_STEAM_ERR_NOT_FOUND, "no Steam folder");

    char backup[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_add_for_account(steam, "20002", &kPlayer, is_running_yes,
                                          "20261106-120000", backup, sizeof(backup), NULL),
                 NK_STEAM_ERR_STEAM_RUNNING, "Steam runs");
    CHECK_STATUS(nk_steam_add_for_account(steam, "..", &kPlayer, is_running_no, "20261106-120000",
                                          backup, sizeof(backup), NULL), NK_STEAM_ERR_ARGUMENT,
                 "a path is not an account");
    CHECK_STATUS(nk_steam_add_for_account(steam, "0", &kPlayer, is_running_no, "20261106-120000",
                                          backup, sizeof(backup), NULL), NK_STEAM_ERR_ARGUMENT,
                 "account 0 is refused");
    CHECK_STATUS(nk_steam_add_for_account(steam, "999", &kPlayer, is_running_no, "20261106-120000",
                                          backup, sizeof(backup), NULL), NK_STEAM_ERR_NOT_FOUND,
                 "an account that is not there");
    CHECK_STATUS(nk_steam_add_for_account(steam, "20002", &kPlayer, is_running_no,
                                          "20261106-120000", backup, sizeof(backup), NULL), NK_STEAM_OK,
                 "an account with no shortcuts file yet gets one");
    CHECK(backup[0] == '\0', "a new file has nothing to back up");
    char vdf[NATIVE_TEST_PATH_MAX];
    snprintf(vdf, sizeof(vdf), "%s/userdata/20002/config/shortcuts.vdf", steam);
    size_t size = 0;
    uint8_t *data = read_bytes(vdf, &size);
    bool found = false;
    CHECK(data && nk_steam_vdf_has_exe(data, size, kPlayer.exe, &found) == NK_STEAM_OK && found,
          "the file was made and lists the player");
    free(data);
    snprintf(vdf, sizeof(vdf), "%s/userdata/30001/config/shortcuts.vdf", steam);
    CHECK(nk_platform_path_missing(vdf), "no other account was touched");
#ifndef _WIN32
    CHECK_STATUS(nk_steam_add_for_account(steam, "30001", &kPlayer, NULL, "20261106-120000",
                                          backup, sizeof(backup), NULL), NK_STEAM_ERR_UNSUPPORTED,
                 "off Windows, without a probe of its own, nothing is written");
    char root[NK_MAX_PATH];
    CHECK_STATUS(nk_steam_find_root(root, sizeof(root)), NK_STEAM_ERR_UNSUPPORTED,
                 "off Windows, Steam is not found by itself");
#endif
}

/* ---- One shortcut per game (#2152) ---- */

static const NkSteamShortcut kGameA = {"Game A", "C:/Games/Nakagawa/bin/nakagawa_player.exe",
                                       "C:/Games/Nakagawa/bin", "--launch-title=ULUS10001"};
static const NkSteamShortcut kGameB = {"Game B", "C:/Games/Nakagawa/bin/nakagawa_player.exe",
                                       "C:/Games/Nakagawa/bin", "--launch-title=ULUS10002"};

/* An entry as an earlier Nakagawa folder left it, after the user played it and tagged it. */
static void raw_game_entry(Raw *r, const char *index, const char *quoted_exe,
                           const char *quoted_dir, const char *options) {
    raw_byte(r, 0x00);
    raw_cstr(r, index);
    raw_int(r, "appid", 0x8000BEEFu);
    raw_str(r, "AppName", "Game A");
    raw_str(r, "exe", quoted_exe);
    raw_str(r, "StartDir", quoted_dir);
    raw_str(r, "icon", "");
    raw_str(r, "LaunchOptions", options);
    raw_int(r, "LastPlayTime", 1700000000u);
    raw_byte(r, 0x00);
    raw_cstr(r, "tags");
    raw_str(r, "0", "favorite");
    raw_byte(r, 0x08);
    raw_byte(r, 0x08);
}

static size_t count_bytes(const uint8_t *hay, size_t size, const char *needle) {
    size_t count = 0;
    size_t length = strlen(needle);
    for (size_t i = 0; length <= size && i + length <= size; i++) {
        if (memcmp(hay + i, needle, length) == 0) count++;
    }
    return count;
}

static void test_each_game_is_its_own_shortcut(void) {
    uint8_t *one = NULL, *two = NULL, *three = NULL, *again = (uint8_t *)1;
    size_t n1 = 0, n2 = 0, n3 = 0, n4 = 7;
    NkSteamChange change = NK_STEAM_CHANGE_NONE;
    CHECK_STATUS(nk_steam_vdf_add(NULL, 0, &kGameA, &one, &n1, &change), NK_STEAM_OK, "game A");
    CHECK(change == NK_STEAM_CHANGE_ADDED, "game A is added");
    CHECK_STATUS(nk_steam_vdf_add(one, n1, &kGameB, &two, &n2, &change), NK_STEAM_OK,
                 "game B: the same executable, other launch options");
    CHECK(change == NK_STEAM_CHANGE_ADDED, "game B is added beside it");
    CHECK_STATUS(nk_steam_vdf_add(two, n2, &kPlayer, &three, &n3, &change), NK_STEAM_OK,
                 "the player itself, with no launch options");
    CHECK(change == NK_STEAM_CHANGE_ADDED, "the bare player is a shortcut of its own");
    CHECK(count_bytes(three, n3, "nakagawa_player.exe") == 3u, "three entries, one executable");
    CHECK_STATUS(nk_steam_vdf_add(three, n3, &kGameA, &again, &n4, &change), NK_STEAM_ERR_EXISTS,
                 "game A again");
    CHECK(again == NULL && n4 == 0u && change == NK_STEAM_CHANGE_NONE,
          "game A again is reported, not duplicated");
    bool found = false;
    CHECK_STATUS(nk_steam_vdf_has_exe(three, n3, kGameA.exe, &found), NK_STEAM_OK, "has exe");
    CHECK(found, "the executable is listed");
    nk_steam_free(one);
    nk_steam_free(two);
    nk_steam_free(three);
}

static void test_a_moved_folder_updates_its_own_entry(void) {
    Raw in, want;
    memset(&in, 0, sizeof(in));
    raw_open(&in);
    raw_entry(&in, "0", "Some Game", "\"C:/Games/A/a.exe\"");
    raw_game_entry(&in, "1", "\"D:/Old/Nakagawa-0.0.1/bin/nakagawa_player.exe\"",
                   "\"D:/Old/Nakagawa-0.0.1/bin\"", "--launch-title=ULUS10001");
    raw_close(&in);
    memset(&want, 0, sizeof(want));
    raw_open(&want);
    raw_entry(&want, "0", "Some Game", "\"C:/Games/A/a.exe\"");
    raw_game_entry(&want, "1", "\"C:/Games/Nakagawa/bin/nakagawa_player.exe\"",
                   "\"C:/Games/Nakagawa/bin\"", "--launch-title=ULUS10001");
    raw_close(&want);
    uint8_t *out = NULL;
    size_t size = 0;
    NkSteamChange change = NK_STEAM_CHANGE_NONE;
    CHECK_STATUS(nk_steam_vdf_add(in.data, in.size, &kGameA, &out, &size, &change), NK_STEAM_OK,
                 "the same game from a new Nakagawa folder");
    CHECK(change == NK_STEAM_CHANGE_UPDATED, "its entry is updated");
    CHECK(out && size == want.size && memcmp(out, want.data, size) == 0,
          "only exe and StartDir change: the app ID, name, play time and tags stay");
    nk_steam_free(out);

    /* Not this game's entry: another game's options, the user's bare player entry, or
       another program with these options. Each leaves its entry and adds one. */
    static const char *const other_options[] = {"--launch-title=ULUS10002", ""};
    for (size_t i = 0; i < 2u; i++) {
        memset(&in, 0, sizeof(in));
        raw_open(&in);
        raw_game_entry(&in, "0", "\"D:/Old/bin/nakagawa_player.exe\"", "\"D:/Old/bin\"",
                       other_options[i]);
        raw_close(&in);
        out = NULL;
        CHECK_STATUS(nk_steam_vdf_add(in.data, in.size, &kGameA, &out, &size, &change),
                     NK_STEAM_OK, "an entry with other launch options");
        CHECK(change == NK_STEAM_CHANGE_ADDED && out && size > in.size &&
                  memcmp(out, in.data, in.size - 2u) == 0,
              "it stays as it was and a new entry follows");
        nk_steam_free(out);
    }
    memset(&in, 0, sizeof(in));
    raw_open(&in);
    raw_game_entry(&in, "0", "\"D:/Tools/other.exe\"", "\"D:/Tools\"",
                   "--launch-title=ULUS10001");
    raw_close(&in);
    out = NULL;
    CHECK_STATUS(nk_steam_vdf_add(in.data, in.size, &kGameA, &out, &size, &change), NK_STEAM_OK,
                 "another program with the same options");
    CHECK(change == NK_STEAM_CHANGE_ADDED, "another program's entry is never taken over");
    nk_steam_free(out);

    /* The bare player is never matched by name alone: the user may have added it by hand. */
    memset(&in, 0, sizeof(in));
    raw_open(&in);
    raw_game_entry(&in, "0", "\"D:/Old/bin/nakagawa_player.exe\"", "\"D:/Old/bin\"", "");
    raw_close(&in);
    out = NULL;
    CHECK_STATUS(nk_steam_vdf_add(in.data, in.size, &kPlayer, &out, &size, &change), NK_STEAM_OK,
                 "the bare player from another folder");
    CHECK(change == NK_STEAM_CHANGE_ADDED, "a bare entry elsewhere is left alone");
    nk_steam_free(out);
}

static void test_file_update_is_backed_up(void) {
    char vdf[NATIVE_TEST_PATH_MAX];
    char backup[NATIVE_TEST_PATH_MAX];
    path_in(vdf, sizeof(vdf), "moved/shortcuts.vdf");
    char dir[NATIVE_TEST_PATH_MAX];
    path_in(dir, sizeof(dir), "moved");
    CHECK(nk_platform_mkdir_p(dir), "folder");
    Raw in;
    memset(&in, 0, sizeof(in));
    raw_open(&in);
    raw_game_entry(&in, "0", "\"D:/Old/bin/nakagawa_player.exe\"", "\"D:/Old/bin\"",
                   "--launch-title=ULUS10001");
    raw_close(&in);
    CHECK(write_bytes(vdf, in.data, in.size), "seed");
    NkSteamChange change = NK_STEAM_CHANGE_NONE;
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kGameA, false, "20261108-090000", backup,
                                      sizeof(backup), &change), NK_STEAM_OK, "update in a file");
    CHECK(change == NK_STEAM_CHANGE_UPDATED, "the file says updated");
    CHECK(backup[0] && file_is(backup, in.data, in.size), "the file from before is the backup");
    size_t size = 0;
    uint8_t *now = read_bytes(vdf, &size);
    CHECK(now && count_bytes(now, size, "nakagawa_player.exe") == 1u &&
              count_bytes(now, size, "C:/Games/Nakagawa/bin") == 2u,
          "one entry, pointing at the new folder");
    free(now);
    CHECK_STATUS(nk_steam_add_to_file(vdf, &kGameA, false, "20261108-090001", backup,
                                      sizeof(backup), &change), NK_STEAM_ERR_EXISTS, "again");
    CHECK(change == NK_STEAM_CHANGE_NONE && backup[0] == '\0', "again writes nothing");
}

/* ---- Account names ---- */

static void test_account_names(void) {
    /* Account 20002 is 76561197960285730, 20003 is 76561197960285731. */
    static const char text[] =
        "\"users\"\n{\n"
        "\t// a comment line\n"
        "\t\"76561197960285730\"\n\t{\n"
        "\t\t\"AccountName\"\t\t\"sign-in-name\"\n"
        "\t\t\"PersonaName\"\t\t\"Ana \\\"Q\\\" Player\"\n"
        "\t\t\"MostRecent\"\t\t\"1\"\n\t}\n"
        "\t\"76561197960285731\"\n\t{\n"
        "\t\t\"PersonaName\"\t\t\"Ren\\tSecond\"\n\t}\n"
        "}\n";
    char name[64];
    CHECK(nk_steam_login_users_name(text, strlen(text), "20002", name, sizeof(name)) &&
              strcmp(name, "Ana \"Q\" Player") == 0,
          "the persona name, escapes read, not the sign-in name");
    CHECK(nk_steam_login_users_name(text, strlen(text), "20003", name, sizeof(name)) &&
              strcmp(name, "RenSecond") == 0,
          "a control character is dropped");
    CHECK(!nk_steam_login_users_name(text, strlen(text), "20004", name, sizeof(name)) &&
              name[0] == '\0',
          "an account not listed has no name");
    CHECK(!nk_steam_login_users_name(text, strlen(text), "x1", name, sizeof(name)),
          "an account ID is digits");
    CHECK(!nk_steam_login_users_name(text, strlen(text) - 3u, "20002", name, sizeof(name)),
          "an unclosed file does not parse");
    static const char outside[] =
        "\"other\" { \"76561197960285730\" { \"PersonaName\" \"Not this\" } }";
    CHECK(!nk_steam_login_users_name(outside, strlen(outside), "20002", name, sizeof(name)),
          "only the users map names accounts");
    /* "Zoë" is Z o 0xC3 0xAB: a 4-byte buffer holds "Zo", never half of the ë. */
    static const char utf8[] = "\"users\" { \"76561197960285730\" { \"PersonaName\" \"Zo\xC3\xAB\" } }";
    char tiny[4];
    CHECK(nk_steam_login_users_name(utf8, strlen(utf8), "20002", tiny, sizeof(tiny)) &&
              strcmp(tiny, "Zo") == 0,
          "a long name is cut at a whole UTF-8 character");

    char steam[NATIVE_TEST_PATH_MAX];
    char config[NATIVE_TEST_PATH_MAX];
    char file[NATIVE_TEST_PATH_MAX];
    path_in(steam, sizeof(steam), "names");
    path_in(config, sizeof(config), "names/config");
    path_in(file, sizeof(file), "names/config/loginusers.vdf");
    CHECK_STATUS(nk_steam_account_name(steam, "20002", name, sizeof(name)),
                 NK_STEAM_ERR_NOT_FOUND, "no loginusers.vdf");
    CHECK(nk_platform_mkdir_p(config), "config folder");
    CHECK(write_bytes(file, (const uint8_t *)text, strlen(text)), "loginusers.vdf");
    CHECK_STATUS(nk_steam_account_name(steam, "20002", name, sizeof(name)), NK_STEAM_OK,
                 "the file's name");
    CHECK(strcmp(name, "Ana \"Q\" Player") == 0, "read from the file");
}

static uint32_t png_test_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0u; i < len; ++i) {
        crc ^= (uint32_t)data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return crc ^ 0xffffffffu;
}

static void png_test_be32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)((v >> 24) & 0xffu);
    dst[1] = (uint8_t)((v >> 16) & 0xffu);
    dst[2] = (uint8_t)((v >> 8) & 0xffu);
    dst[3] = (uint8_t)(v & 0xffu);
}

static bool png_test_chunk(uint8_t *out, size_t *pos, size_t cap, const char type[4],
                           const uint8_t *payload, size_t len) {
    if (*pos + 12u + len > cap) return false;
    png_test_be32(out + *pos, (uint32_t)len);
    memcpy(out + *pos + 4u, type, 4u);
    if (len > 0u && payload != NULL) memcpy(out + *pos + 8u, payload, len);
    png_test_be32(out + *pos + 8u + len, png_test_crc32(out + *pos + 4u, 4u + len));
    *pos += 12u + len;
    return true;
}

static size_t make_synthetic_png(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t a, uint8_t *out, size_t cap) {
    static const uint8_t k_sig[8] = {0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au};
    size_t row_bytes = 1u + (size_t)w * 4u;
    size_t raw_len = (size_t)h * row_bytes;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    uint8_t *zlib = (uint8_t *)malloc(raw_len + 64u);
    uint8_t ihdr[13];
    size_t pos = 0u, zpos = 0u;
    uint32_t s1 = 1u, s2 = 0u;
    if (raw == NULL || zlib == NULL || cap < 64u || raw_len > 65535u) {
        free(raw);
        free(zlib);
        return 0u;
    }
    for (uint32_t y = 0u; y < h; ++y) {
        uint8_t *row = raw + (size_t)y * row_bytes;
        row[0] = 0u;
        for (uint32_t x = 0u; x < w; ++x) {
            row[1u + (size_t)x * 4u + 0u] = r;
            row[1u + (size_t)x * 4u + 1u] = g;
            row[1u + (size_t)x * 4u + 2u] = b;
            row[1u + (size_t)x * 4u + 3u] = a;
        }
    }
    for (size_t i = 0u; i < raw_len; ++i) {
        s1 = (s1 + raw[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    zlib[zpos++] = 0x78u;
    zlib[zpos++] = 0x01u;
    zlib[zpos++] = 0x01u;
    zlib[zpos++] = (uint8_t)(raw_len & 0xffu);
    zlib[zpos++] = (uint8_t)((raw_len >> 8) & 0xffu);
    zlib[zpos++] = (uint8_t)((~raw_len) & 0xffu);
    zlib[zpos++] = (uint8_t)(((~raw_len) >> 8) & 0xffu);
    memcpy(zlib + zpos, raw, raw_len);
    zpos += raw_len;
    png_test_be32(zlib + zpos, (s2 << 16) | s1);
    zpos += 4u;

    memcpy(out, k_sig, 8u);
    pos = 8u;
    png_test_be32(ihdr + 0u, w);
    png_test_be32(ihdr + 4u, h);
    ihdr[8] = 8u;
    ihdr[9] = 6u;
    ihdr[10] = 0u;
    ihdr[11] = 0u;
    ihdr[12] = 0u;
    if (!png_test_chunk(out, &pos, cap, "IHDR", ihdr, sizeof(ihdr)) ||
        !png_test_chunk(out, &pos, cap, "IDAT", zlib, zpos) ||
        !png_test_chunk(out, &pos, cap, "IEND", NULL, 0u)) {
        pos = 0u;
    }
    free(raw);
    free(zlib);
    return pos;
}

static void join_rel_path(char *out, size_t cap, const char *base, const char *rel) {
    int n = snprintf(out, cap, "%s/%s", base, rel);
    if (n <= 0 || (size_t)n >= cap) out[0] = '\0';
}

static void grid_file_in(char *out, size_t cap, const char *grid_dir, uint32_t app_id,
                         const char *suffix) {
    int n = snprintf(out, cap, "%s/%lu%s", grid_dir, (unsigned long)app_id, suffix);
    if (n <= 0 || (size_t)n >= cap) out[0] = '\0';
}

static bool path_matches(const char *a, const char *b) {
    if (a == NULL || b == NULL) return false;
    while (*a != '\0' && *b != '\0') {
        char ca = (*a == '\\') ? '/' : *a;
        char cb = (*b == '\\') ? '/' : *b;
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void test_grid_slot_paths_and_suffixes(void) {
    char out[NK_MAX_PATH];
    uint32_t app_id = nk_steam_shortcut_app_id(kGameA.exe, kGameA.app_name);
    char expect_tail[64];

    CHECK(strcmp(nk_steam_grid_slot_suffix(NK_STEAM_GRID_SLOT_ICON), "_icon.png") == 0,
          "icon suffix");
    CHECK(strcmp(nk_steam_grid_slot_suffix(NK_STEAM_GRID_SLOT_WIDE), ".png") == 0,
          "wide capsule suffix");
    CHECK(strcmp(nk_steam_grid_slot_suffix(NK_STEAM_GRID_SLOT_HERO), "_hero.png") == 0,
          "hero suffix");
    CHECK(nk_steam_grid_slot_suffix((NkSteamGridSlot)99) == NULL, "invalid suffix returns NULL");

    CHECK(nk_steam_grid_slot_path("/steam/userdata/100/config/grid/", app_id,
                                  NK_STEAM_GRID_SLOT_WIDE, out, sizeof(out)),
          "wide slot path trims trailing slash");
    snprintf(expect_tail, sizeof(expect_tail), "%c%lu.png", nk_platform_path_separator(),
             (unsigned long)app_id);
    CHECK(strstr(out, expect_tail) != NULL, "wide slot path ends with <appid>.png");

    CHECK(nk_steam_grid_slot_path("/grid", app_id, NK_STEAM_GRID_SLOT_ICON, out, sizeof(out)),
          "icon slot path formats");
    snprintf(expect_tail, sizeof(expect_tail), "%lu_icon.png", (unsigned long)app_id);
    CHECK(strstr(out, expect_tail) != NULL, "icon slot contains <appid>_icon.png");

    CHECK(nk_steam_grid_slot_path("/grid", app_id, NK_STEAM_GRID_SLOT_HERO, out, sizeof(out)),
          "hero slot path formats");
    snprintf(expect_tail, sizeof(expect_tail), "%lu_hero.png", (unsigned long)app_id);
    CHECK(strstr(out, expect_tail) != NULL, "hero slot contains <appid>_hero.png");

    CHECK(!nk_steam_grid_slot_path(NULL, app_id, NK_STEAM_GRID_SLOT_WIDE, out, sizeof(out)),
          "NULL dir refused");
    CHECK(!nk_steam_grid_slot_path("/grid", 0u, NK_STEAM_GRID_SLOT_WIDE, out, sizeof(out)),
          "zero app_id refused");
    CHECK(!nk_steam_grid_slot_path("/grid", app_id, NK_STEAM_GRID_SLOT_WIDE, out, 8u),
          "small buffer refused");
}

static void test_grid_writes_and_reads_back(void) {
    char grid_dir[NK_MAX_PATH];
    char p_icon[NK_MAX_PATH * 2], p_wide[NK_MAX_PATH * 2], p_hero[NK_MAX_PATH * 2];
    char p_port[NK_MAX_PATH * 2], p_logo[NK_MAX_PATH * 2], p_tmp[NK_MAX_PATH * 2];
    uint8_t icon0[2048], pic1[2048];
    size_t icon0_len = make_synthetic_png(16u, 9u, 0x10u, 0x80u, 0xd0u, 0xffu, icon0, sizeof(icon0));
    size_t pic1_len = make_synthetic_png(24u, 14u, 0xe0u, 0x50u, 0x30u, 0xffu, pic1, sizeof(pic1));
    uint32_t app_id = nk_steam_shortcut_app_id(kGameA.exe, kGameA.app_name);
    NkSteamGridArt art;
    NkSteamGridResult res;

    CHECK(icon0_len > 0u && pic1_len > 0u, "synthetic PNG generation");
    path_in(grid_dir, sizeof(grid_dir), "grid_basic/config/grid");
    grid_file_in(p_icon, sizeof(p_icon), grid_dir, app_id, "_icon.png");
    grid_file_in(p_wide, sizeof(p_wide), grid_dir, app_id, ".png");
    grid_file_in(p_hero, sizeof(p_hero), grid_dir, app_id, "_hero.png");
    grid_file_in(p_port, sizeof(p_port), grid_dir, app_id, "p.png");
    grid_file_in(p_logo, sizeof(p_logo), grid_dir, app_id, "_logo.png");
    grid_file_in(p_tmp, sizeof(p_tmp), grid_dir, app_id, "_icon.png.nakagawa-tmp");

    art.icon0_png = icon0;
    art.icon0_size = icon0_len;
    art.pic1_png = pic1;
    art.pic1_size = pic1_len;
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_OK, "both ICON0 and PIC1 write cleanly");
    CHECK(res.written_mask ==
              (NK_STEAM_GRID_SLOT_ICON | NK_STEAM_GRID_SLOT_WIDE | NK_STEAM_GRID_SLOT_HERO),
          "all three landscape slots marked written");
    CHECK(res.kept_mask == 0u && res.absent_mask == 0u, "none kept or absent");
    CHECK(file_is(p_icon, icon0, icon0_len), "icon slot gets ICON0.PNG");
    CHECK(file_is(p_wide, icon0, icon0_len), "wide capsule slot gets ICON0.PNG");
    CHECK(file_is(p_hero, pic1, pic1_len), "hero slot gets PIC1.PNG");
    CHECK(nk_platform_path_missing(p_port), "portrait capsule left empty");
    CHECK(nk_platform_path_missing(p_logo), "logo slot left empty");
    CHECK(nk_platform_path_missing(p_tmp), "temp file cleaned up");

    /* PIC1-only fallback for wide capsule; ICON0 stays empty. */
    path_in(grid_dir, sizeof(grid_dir), "grid_pic_only");
    grid_file_in(p_icon, sizeof(p_icon), grid_dir, app_id, "_icon.png");
    grid_file_in(p_wide, sizeof(p_wide), grid_dir, app_id, ".png");
    grid_file_in(p_hero, sizeof(p_hero), grid_dir, app_id, "_hero.png");
    art.icon0_png = NULL;
    art.icon0_size = 0u;
    art.pic1_png = pic1;
    art.pic1_size = pic1_len;
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_OK, "PIC1-only writes wide capsule fallback and hero");
    CHECK(res.written_mask == (NK_STEAM_GRID_SLOT_WIDE | NK_STEAM_GRID_SLOT_HERO) &&
              res.absent_mask == NK_STEAM_GRID_SLOT_ICON,
          "PIC1-only masks match");
    CHECK(nk_platform_path_missing(p_icon), "absent ICON0 writes no _icon.png");
    CHECK(file_is(p_wide, pic1, pic1_len), "wide capsule falls back to PIC1 when ICON0 absent");
    CHECK(file_is(p_hero, pic1, pic1_len), "hero gets PIC1");

    /* ICON0-only leaves hero absent. */
    path_in(grid_dir, sizeof(grid_dir), "grid_icon_only");
    grid_file_in(p_icon, sizeof(p_icon), grid_dir, app_id, "_icon.png");
    grid_file_in(p_wide, sizeof(p_wide), grid_dir, app_id, ".png");
    grid_file_in(p_hero, sizeof(p_hero), grid_dir, app_id, "_hero.png");
    art.icon0_png = icon0;
    art.icon0_size = icon0_len;
    art.pic1_png = NULL;
    art.pic1_size = 0u;
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_OK, "ICON0-only writes icon and wide capsule");
    CHECK(res.written_mask == (NK_STEAM_GRID_SLOT_ICON | NK_STEAM_GRID_SLOT_WIDE) &&
              res.absent_mask == NK_STEAM_GRID_SLOT_HERO,
          "ICON0-only masks match");
    CHECK(file_is(p_icon, icon0, icon0_len) && file_is(p_wide, icon0, icon0_len),
          "ICON0 files match");
    CHECK(nk_platform_path_missing(p_hero), "absent PIC1 writes no _hero.png");
}

static void test_grid_steam_running_and_overwrite_guards(void) {
    char grid_dir[NK_MAX_PATH], never_dir[NK_MAX_PATH];
    char p_icon[NK_MAX_PATH * 2], p_wide[NK_MAX_PATH * 2], p_hero[NK_MAX_PATH * 2];
    char p_orig[NK_MAX_PATH * 2];
    uint8_t icon0[2048], pic1[2048], custom_png[2048];
    size_t icon0_len = make_synthetic_png(10u, 6u, 1u, 2u, 3u, 255u, icon0, sizeof(icon0));
    size_t pic1_len = make_synthetic_png(12u, 7u, 4u, 5u, 6u, 255u, pic1, sizeof(pic1));
    size_t custom_len = make_synthetic_png(8u, 8u, 9u, 9u, 9u, 255u, custom_png, sizeof(custom_png));
    uint32_t app_id = nk_steam_shortcut_app_id(kGameB.exe, kGameB.app_name);
    NkSteamGridArt art = {icon0, icon0_len, pic1, pic1_len};
    NkSteamGridResult res;

    /* Running Steam creates nothing and refuses cleanly. */
    path_in(never_dir, sizeof(never_dir), "grid_while_running/userdata/100/config/grid");
    CHECK_STATUS(nk_steam_write_grid_art(never_dir, app_id, &art, true,
                                         NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_ERR_STEAM_RUNNING, "running Steam refuses grid write");
    CHECK(nk_platform_path_missing(never_dir) && res.written_mask == 0u &&
              strstr(res.detail, "running") != NULL,
          "running Steam creates no folder and explains refusal");

    /* Pre-existing custom hero must never be overwritten silently. */
    path_in(grid_dir, sizeof(grid_dir), "grid_overwrite");
    nk_platform_mkdir_p(grid_dir);
    grid_file_in(p_icon, sizeof(p_icon), grid_dir, app_id, "_icon.png");
    grid_file_in(p_wide, sizeof(p_wide), grid_dir, app_id, ".png");
    grid_file_in(p_hero, sizeof(p_hero), grid_dir, app_id, "_hero.png");
    grid_file_in(p_orig, sizeof(p_orig), grid_dir, app_id, "_hero.png.nakagawa-orig");
    CHECK(write_bytes(p_hero, custom_png, custom_len), "seed custom hero");

    /* Mode FAIL_IF_EXISTS leaves the whole grid folder untouched. */
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_FAIL_IF_EXISTS, &res),
                 NK_STEAM_ERR_EXISTS, "FAIL_IF_EXISTS refuses pre-existing hero");
    CHECK(path_matches(res.failed_path, p_hero) && nk_platform_path_missing(p_icon) &&
              nk_platform_path_missing(p_wide) && file_is(p_hero, custom_png, custom_len),
          "FAIL_IF_EXISTS names existing file and leaves everything untouched");

    /* Mode KEEP_EXISTING writes missing slots, preserves existing hero, and reports it. */
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_OK, "KEEP_EXISTING succeeds while preserving custom hero");
    CHECK(res.written_mask == (NK_STEAM_GRID_SLOT_ICON | NK_STEAM_GRID_SLOT_WIDE) &&
              res.kept_mask == NK_STEAM_GRID_SLOT_HERO &&
              file_is(p_icon, icon0, icon0_len) && file_is(p_wide, icon0, icon0_len) &&
              file_is(p_hero, custom_png, custom_len) &&
              strstr(res.detail, "_hero.png") != NULL,
          "KEEP_EXISTING wrote missing slots, left custom hero intact, and named kept file");

    /* Mode OVERWRITE explicitly replaces the pre-existing file cleanly. */
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_OVERWRITE, &res),
                 NK_STEAM_OK, "OVERWRITE replaces all slots cleanly");
    CHECK(res.written_mask ==
              (NK_STEAM_GRID_SLOT_ICON | NK_STEAM_GRID_SLOT_WIDE | NK_STEAM_GRID_SLOT_HERO) &&
              res.kept_mask == 0u && file_is(p_hero, pic1, pic1_len) &&
              nk_platform_path_missing(p_orig),
          "OVERWRITE replaced hero and cleaned rollback copy");

    /* A rollback copy an earlier run could not put back (or a crash left) may be the user's
       only copy of their own picture: OVERWRITE refuses instead of truncating it. */
    CHECK(write_bytes(p_orig, custom_png, custom_len), "seed a leftover rollback copy");
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_OVERWRITE, &res),
                 NK_STEAM_ERR_BACKUP, "OVERWRITE refuses over a leftover rollback copy");
    CHECK(path_matches(res.failed_path, p_orig) && res.written_mask == 0u &&
              file_is(p_orig, custom_png, custom_len) && file_is(p_hero, pic1, pic1_len) &&
              file_is(p_icon, icon0, icon0_len) && file_is(p_wide, icon0, icon0_len),
          "leftover rollback copy is kept byte for byte and named, and no slot changed");
}

static void test_grid_invalid_pngs_and_rollback(void) {
    char grid_dir[NK_MAX_PATH];
    char p_icon[NK_MAX_PATH * 2], p_wide[NK_MAX_PATH * 2], p_hero[NK_MAX_PATH * 2];
    char p_tmp_block[NK_MAX_PATH * 2], p_icon_tmp[NK_MAX_PATH * 2], p_orig[NK_MAX_PATH * 2];
    uint8_t good[2048], old_hero[2048], bad_sig[64];
    uint8_t *oversized;
    size_t good_len = make_synthetic_png(8u, 8u, 7u, 8u, 9u, 255u, good, sizeof(good));
    size_t old_len = make_synthetic_png(6u, 6u, 20u, 30u, 40u, 255u, old_hero, sizeof(old_hero));
    uint32_t app_id = nk_steam_shortcut_app_id(kPlayer.exe, kPlayer.app_name);
    NkSteamGridArt art;
    NkSteamGridResult res;

    path_in(grid_dir, sizeof(grid_dir), "grid_invalid");
    nk_platform_mkdir_p(grid_dir);
    grid_file_in(p_hero, sizeof(p_hero), grid_dir, app_id, "_hero.png");
    CHECK(write_bytes(p_hero, old_hero, old_len), "seed existing hero before negative checks");

    /* Corrupt non-PNG bytes */
    memset(bad_sig, 'X', sizeof(bad_sig));
    art.icon0_png = bad_sig;
    art.icon0_size = sizeof(bad_sig);
    art.pic1_png = NULL;
    art.pic1_size = 0u;
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_OVERWRITE, &res),
                 NK_STEAM_ERR_CORRUPT, "non-PNG bytes refused");
    CHECK(file_is(p_hero, old_hero, old_len) && strstr(res.detail, "ICON0.PNG") != NULL,
          "existing hero untouched and corrupt slot named");

    /* Zero-width IHDR PNG refused */
    {
        uint8_t zero_w[2048];
        memcpy(zero_w, good, good_len);
        png_test_be32(zero_w + 16u, 0u);
        art.icon0_png = zero_w;
        art.icon0_size = good_len;
        CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                             NK_STEAM_GRID_OVERWRITE, &res),
                     NK_STEAM_ERR_CORRUPT, "zero-width IHDR refused");
    }

    /* Trailing bytes after IEND refused */
    {
        uint8_t trailing[2048];
        memcpy(trailing, good, good_len);
        trailing[good_len] = 0x42u;
        art.icon0_png = trailing;
        art.icon0_size = good_len + 1u;
        CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                             NK_STEAM_GRID_OVERWRITE, &res),
                     NK_STEAM_ERR_CORRUPT, "trailing garbage after IEND refused");
    }

    /* Oversized PNG (> NK_STEAM_GRID_MAX_BYTES) refused */
    oversized = (uint8_t *)calloc(1u, NK_STEAM_GRID_MAX_BYTES + 1u);
    CHECK(oversized != NULL, "allocate oversized buffer");
    memcpy(oversized, good, good_len);
    art.icon0_png = oversized;
    art.icon0_size = NK_STEAM_GRID_MAX_BYTES + 1u;
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_OVERWRITE, &res),
                 NK_STEAM_ERR_TOO_LARGE, "oversized PNG refused");
    free(oversized);

    /* Failure mid-batch (blocking dir at wide tmp path): rolls back every file and leaves
       pre-existing hero intact while naming the failing path. */
    path_in(grid_dir, sizeof(grid_dir), "grid_rollback");
    nk_platform_mkdir_p(grid_dir);
    grid_file_in(p_icon, sizeof(p_icon), grid_dir, app_id, "_icon.png");
    grid_file_in(p_wide, sizeof(p_wide), grid_dir, app_id, ".png");
    grid_file_in(p_hero, sizeof(p_hero), grid_dir, app_id, "_hero.png");
    grid_file_in(p_tmp_block, sizeof(p_tmp_block), grid_dir, app_id, ".png.nakagawa-tmp");
    grid_file_in(p_icon_tmp, sizeof(p_icon_tmp), grid_dir, app_id, "_icon.png.nakagawa-tmp");
    grid_file_in(p_orig, sizeof(p_orig), grid_dir, app_id, "_hero.png.nakagawa-orig");
    CHECK(write_bytes(p_hero, old_hero, old_len), "seed prior hero");
    CHECK(nk_platform_mkdir_p(p_tmp_block), "plant blocking temp dir");

    art.icon0_png = good;
    art.icon0_size = good_len;
    art.pic1_png = good;
    art.pic1_size = good_len;
    CHECK_STATUS(nk_steam_write_grid_art(grid_dir, app_id, &art, false,
                                         NK_STEAM_GRID_OVERWRITE, &res),
                 NK_STEAM_ERR_WRITE, "mid-batch blocker fails closed");
    CHECK(path_matches(res.failed_path, p_tmp_block) && res.written_mask == 0u &&
              nk_platform_path_missing(p_icon) && nk_platform_path_missing(p_icon_tmp) &&
              nk_platform_path_missing(p_wide) && nk_platform_path_missing(p_orig) &&
              file_is(p_hero, old_hero, old_len),
          "mid-batch failure names blocker, cleans temps, and keeps prior hero");
}

static void test_add_for_account_with_art_and_rollback(void) {
    char steam[NK_MAX_PATH], cfg[NK_MAX_PATH * 2], grid_dir[NK_MAX_PATH * 2];
    char vdf[NK_MAX_PATH * 2], backup[NK_MAX_PATH];
    char p_icon[NK_MAX_PATH * 2], p_wide[NK_MAX_PATH * 2], p_hero[NK_MAX_PATH * 2];
    uint8_t icon0[2048], pic1[2048], custom_png[2048];
    size_t icon0_len = make_synthetic_png(12u, 6u, 0x11u, 0x22u, 0x33u, 255u, icon0, sizeof(icon0));
    size_t pic1_len = make_synthetic_png(16u, 9u, 0x44u, 0x55u, 0x66u, 255u, pic1, sizeof(pic1));
    size_t custom_len = make_synthetic_png(8u, 4u, 0x77u, 0x88u, 0x99u, 255u, custom_png, sizeof(custom_png));
    NkSteamGridArt art = {icon0, icon0_len, pic1, pic1_len};
    NkSteamGridResult res;
    NkSteamChange ch = NK_STEAM_CHANGE_NONE;

    /* 1. Fresh account: adds shortcut + artwork under shortcut's app_id. */
    path_in(steam, sizeof(steam), "steam_acct_art");
    join_rel_path(cfg, sizeof(cfg), steam, "userdata/30001/config");
    nk_platform_mkdir_p(cfg);
    CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30001", &kGameA, is_running_yes,
                                                   "20261115-100000", backup, sizeof(backup), &ch,
                                                   &art, NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_ERR_STEAM_RUNNING, "running Steam blocks combined add+art");
    join_rel_path(vdf, sizeof(vdf), cfg, "shortcuts.vdf");
    join_rel_path(grid_dir, sizeof(grid_dir), cfg, "grid");
    CHECK(nk_platform_path_missing(vdf) && nk_platform_path_missing(grid_dir),
          "running Steam created neither shortcuts.vdf nor grid");

    CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30001", &kGameA, is_running_no,
                                                   "20261115-100001", backup, sizeof(backup), &ch,
                                                   &art, NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_OK, "combined add+art succeeds");
    CHECK(ch == NK_STEAM_CHANGE_ADDED &&
              res.app_id == nk_steam_shortcut_app_id(kGameA.exe, kGameA.app_name) &&
              res.written_mask ==
                  (NK_STEAM_GRID_SLOT_ICON | NK_STEAM_GRID_SLOT_WIDE | NK_STEAM_GRID_SLOT_HERO),
          "combined add+art derives app_id and writes all 3 slots");
    grid_file_in(p_icon, sizeof(p_icon), grid_dir, res.app_id, "_icon.png");
    grid_file_in(p_wide, sizeof(p_wide), grid_dir, res.app_id, ".png");
    grid_file_in(p_hero, sizeof(p_hero), grid_dir, res.app_id, "_hero.png");
    CHECK(file_is(p_icon, icon0, icon0_len) && file_is(p_wide, icon0, icon0_len) &&
              file_is(p_hero, pic1, pic1_len),
          "combined add+art wrote expected bytes");

    /* 2. Moved-folder update preserves the shortcut's existing app_id in shortcuts.vdf. */
    {
        NkSteamShortcut moved = kGameA;
        moved.exe = "C:/Games/Nakagawa-v2/bin/nakagawa_player.exe";
        moved.start_dir = "C:/Games/Nakagawa-v2/bin";
        uint8_t custom_pic[2048];
        size_t custom_pic_len =
            make_synthetic_png(14u, 8u, 0xaa, 0xbb, 0xcc, 255u, custom_pic, sizeof(custom_pic));
        NkSteamGridArt art2 = {custom_pic, custom_pic_len, pic1, pic1_len};
        CHECK(write_bytes(p_hero, custom_png, custom_len), "customize existing hero");
        CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30001", &moved, is_running_no,
                                                       "20261115-100002", backup, sizeof(backup),
                                                       &ch, &art2, NK_STEAM_GRID_KEEP_EXISTING,
                                                       &res),
                     NK_STEAM_OK, "moved update preserves existing app_id and custom art");
        CHECK(ch == NK_STEAM_CHANGE_UPDATED &&
                  res.app_id == nk_steam_shortcut_app_id(kGameA.exe, kGameA.app_name) &&
                  res.kept_mask == (NK_STEAM_GRID_SLOT_ICON | NK_STEAM_GRID_SLOT_WIDE |
                                    NK_STEAM_GRID_SLOT_HERO) &&
                  res.written_mask == 0u && file_is(p_hero, custom_png, custom_len),
              "moved update kept original app_id and untouched existing artwork");
    }

    /* 3. Grid failure after shortcuts.vdf mutation rolls back shortcuts.vdf cleanly. */
    path_in(steam, sizeof(steam), "steam_acct_art_rollback_new");
    join_rel_path(cfg, sizeof(cfg), steam, "userdata/30002/config");
    nk_platform_mkdir_p(cfg);
    join_rel_path(vdf, sizeof(vdf), cfg, "shortcuts.vdf");
    join_rel_path(grid_dir, sizeof(grid_dir), cfg, "grid");
    CHECK(write_bytes(grid_dir, (const uint8_t *)"blocker", 7u), "block config/grid with file");
    CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30002", &kGameA, is_running_no,
                                                   "20261115-100003", backup, sizeof(backup), &ch,
                                                   &art, NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_ERR_WRITE, "file blocking grid dir fails closed");
    CHECK(path_matches(res.failed_path, grid_dir) && ch == NK_STEAM_CHANGE_NONE &&
              backup[0] == '\0' && nk_platform_path_missing(vdf),
          "new shortcuts.vdf is removed when grid creation fails");

    /* 4. Existing shortcuts.vdf rollback: pre-existing VDF bytes are restored on grid failure. */
    path_in(steam, sizeof(steam), "steam_acct_art_rollback_existing");
    join_rel_path(cfg, sizeof(cfg), steam, "userdata/30003/config");
    nk_platform_mkdir_p(cfg);
    join_rel_path(vdf, sizeof(vdf), cfg, "shortcuts.vdf");
    join_rel_path(grid_dir, sizeof(grid_dir), cfg, "grid");
    {
        Raw existing;
        uint32_t app_id_b = nk_steam_shortcut_app_id(kGameB.exe, kGameB.app_name);
        char slot_blocker[NK_MAX_PATH * 2];
        two_entries(&existing);
        CHECK(write_bytes(vdf, existing.data, existing.size), "seed existing shortcuts.vdf");
        grid_file_in(slot_blocker, sizeof(slot_blocker), grid_dir, app_id_b, ".png");
        CHECK(nk_platform_mkdir_p(slot_blocker), "block Game B wide slot with directory");
        CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30003", &kGameB, is_running_no,
                                                       "20261115-100004", backup, sizeof(backup),
                                                       &ch, &art, NK_STEAM_GRID_KEEP_EXISTING,
                                                       &res),
                     NK_STEAM_ERR_WRITE, "slot dir blocker fails closed after existing VDF");
        CHECK(path_matches(res.failed_path, slot_blocker) && ch == NK_STEAM_CHANGE_NONE &&
                  backup[0] == '\0' && file_is(vdf, existing.data, existing.size),
              "existing shortcuts.vdf restored byte-for-byte on grid failure");
    }
}

/* One Steam account folder for the tests below: `<root>/<leaf>/userdata/<id>/config`. */
static void art_account(const char *leaf, const char *id, char *steam, size_t steam_cap,
                        char *vdf, size_t vdf_cap) {
    char cfg[NK_MAX_PATH * 2];
    char rel[64];
    path_in(steam, steam_cap, leaf);
    snprintf(rel, sizeof(rel), "userdata/%s/config", id);
    join_rel_path(cfg, sizeof(cfg), steam, rel);
    CHECK(nk_platform_mkdir_p(cfg), "account config folder");
    join_rel_path(vdf, vdf_cap, cfg, "shortcuts.vdf");
}

/* Every Nakagawa shortcut runs the same player; only its launch options name the game. Each
 * game added in turn gets artwork under its own app ID (#2630 review, finding 1). */
static void test_grid_each_title_gets_its_own_app_id(void) {
    static const NkSteamShortcut kGameC = {"Game C", "C:/Games/Nakagawa/bin/nakagawa_player.exe",
                                           "C:/Games/Nakagawa/bin", "--launch-title=ULUS10003"};
    const NkSteamShortcut *games[3] = {&kGameA, &kGameB, &kGameC};
    char steam[NK_MAX_PATH], vdf[NK_MAX_PATH * 2], backup[NK_MAX_PATH];
    char p_icon[NK_MAX_PATH * 2], p_hero[NK_MAX_PATH * 2];
    uint8_t icon0[3][2048], pic1[3][2048];
    size_t icon0_len[3], pic1_len[3];
    uint32_t ids[3];
    NkSteamGridResult res;
    NkSteamChange ch = NK_STEAM_CHANGE_NONE;

    art_account("steam_art_three_titles", "30010", steam, sizeof(steam), vdf, sizeof(vdf));
    for (size_t i = 0u; i < 3u; ++i) {
        NkSteamGridArt art;
        char stamp[32];
        icon0_len[i] = make_synthetic_png(10u + (uint32_t)i, 6u, (uint8_t)(0x10u * i), 0x20u,
                                          0x30u, 255u, icon0[i], sizeof(icon0[i]));
        pic1_len[i] = make_synthetic_png(12u + (uint32_t)i, 7u, 0x40u, (uint8_t)(0x10u * i),
                                         0x50u, 255u, pic1[i], sizeof(pic1[i]));
        art.icon0_png = icon0[i];
        art.icon0_size = icon0_len[i];
        art.pic1_png = pic1[i];
        art.pic1_size = pic1_len[i];
        ids[i] = nk_steam_shortcut_app_id(games[i]->exe, games[i]->app_name);
        snprintf(stamp, sizeof(stamp), "20261115-11000%u", (unsigned)i);
        CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30010", games[i], is_running_no,
                                                       stamp, backup, sizeof(backup), &ch, &art,
                                                       NK_STEAM_GRID_KEEP_EXISTING, &res),
                     NK_STEAM_OK, "each title is added with its artwork");
        CHECK(ch == NK_STEAM_CHANGE_ADDED, "each title is a shortcut of its own");
        CHECK(res.app_id == ids[i], "artwork goes under this title's own app ID");
        CHECK(res.written_mask == (NK_STEAM_GRID_SLOT_ICON | NK_STEAM_GRID_SLOT_WIDE |
                                   NK_STEAM_GRID_SLOT_HERO) && res.kept_mask == 0u,
              "every slot of a new title is written, none kept from another title");
    }
    CHECK(ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2], "three distinct app IDs");
    {
        size_t size = 0u;
        uint8_t *bytes = read_bytes(vdf, &size);
        uint32_t found = 0u;
        NkSteamShortcut other = kGameB;
        CHECK(bytes != NULL && nk_steam_vdf_find_app_id(bytes, size, &kGameB, &found) &&
                  found == ids[1],
              "the lookup finds game B's own entry, not game A's");
        other.launch_options = "--launch-title=ULUS19999";
        CHECK(bytes != NULL && !nk_steam_vdf_find_app_id(bytes, size, &other, &found) &&
                  found == 0u,
              "the same player with another game's options is not found");
        free(bytes);
    }
    for (size_t i = 0u; i < 3u; ++i) {
        {
            char grid_dir[NK_MAX_PATH * 2];
            join_rel_path(grid_dir, sizeof(grid_dir), steam, "userdata/30010/config/grid");
            grid_file_in(p_icon, sizeof(p_icon), grid_dir, ids[i], "_icon.png");
            grid_file_in(p_hero, sizeof(p_hero), grid_dir, ids[i], "_hero.png");
        }
        CHECK(file_is(p_icon, icon0[i], icon0_len[i]) && file_is(p_hero, pic1[i], pic1_len[i]),
              "each title's own ICON0 and PIC1 sit under its own app ID");
    }
}

/* A failed grid write after shortcuts.vdf changed, when the list cannot be put back: the
 * verified backup is the user's only copy and stays, named (#2630 review, finding 2). */
static void test_grid_failed_restore_keeps_the_backup(void) {
    char steam[NK_MAX_PATH], vdf[NK_MAX_PATH * 2], backup[NK_MAX_PATH];
    char grid_dir[NK_MAX_PATH * 2], slot_blocker[NK_MAX_PATH * 2];
    char restore_blocker[NK_MAX_PATH * 3];
    uint8_t icon0[2048];
    size_t icon0_len = make_synthetic_png(9u, 5u, 1u, 2u, 3u, 255u, icon0, sizeof(icon0));
    NkSteamGridArt art = {icon0, icon0_len, NULL, 0u};
    NkSteamGridResult res;
    NkSteamChange ch = NK_STEAM_CHANGE_NONE;
    Raw existing;

    art_account("steam_art_restore_fails", "30011", steam, sizeof(steam), vdf, sizeof(vdf));
    join_rel_path(grid_dir, sizeof(grid_dir), steam, "userdata/30011/config/grid");
    two_entries(&existing);
    CHECK(write_bytes(vdf, existing.data, existing.size), "seed shortcuts.vdf");
    grid_file_in(slot_blocker, sizeof(slot_blocker), grid_dir,
                 nk_steam_shortcut_app_id(kGameB.exe, kGameB.app_name), ".png");
    CHECK(nk_platform_mkdir_p(slot_blocker), "a folder blocks the wide capsule");
    snprintf(restore_blocker, sizeof(restore_blocker), "%s.nakagawa-restore", vdf);
    CHECK(nk_platform_mkdir_p(restore_blocker), "a folder blocks putting the list back");
    CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30011", &kGameB, is_running_no,
                                                   "20261115-110100", backup, sizeof(backup), &ch,
                                                   &art, NK_STEAM_GRID_KEEP_EXISTING, &res),
                 NK_STEAM_ERR_WRITE, "the artwork write fails");
    CHECK(backup[0] != '\0', "the backup is still named");
    CHECK(backup[0] != '\0' && file_is(backup, existing.data, existing.size),
          "the backup of the list from before is kept, byte for byte");
    CHECK(ch == NK_STEAM_CHANGE_ADDED && res.restore_failed,
          "the list still holds the new shortcut, and says so");
}

/* The game's pictures are optional: one that is malformed, too large, empty or missing never
 * stops the shortcut; it is skipped and the rest is used (#2630 review, finding 3). */
static void test_grid_bad_art_never_blocks_the_shortcut(void) {
    char steam[NK_MAX_PATH], vdf[NK_MAX_PATH * 2], backup[NK_MAX_PATH];
    char grid_dir[NK_MAX_PATH * 2], p_icon[NK_MAX_PATH * 2], p_hero[NK_MAX_PATH * 2];
    uint8_t good[2048], trailing[2048];
    size_t good_len = make_synthetic_png(8u, 8u, 7u, 8u, 9u, 255u, good, sizeof(good));
    uint8_t *oversized = (uint8_t *)calloc(1u, NK_STEAM_GRID_MAX_BYTES + 1u);
    static const uint8_t not_png[] = "NOTAPNG";
    NkSteamGridResult res;
    NkSteamChange ch = NK_STEAM_CHANGE_NONE;
    struct {
        const char *what;
        const uint8_t *icon0;
        size_t icon0_size;
    } cases[4];
    size_t n = 0u;
    uint8_t *vdf_bytes;
    size_t vdf_size = 0u;

    CHECK(oversized != NULL, "allocate oversized buffer");
    if (oversized == NULL) return;
    memcpy(oversized, good, good_len);
    memcpy(trailing, good, good_len);
    trailing[good_len] = 0x42u;
    cases[n].what = "not a PNG";
    cases[n].icon0 = not_png;
    cases[n++].icon0_size = sizeof(not_png) - 1u;
    cases[n].what = "bytes after IEND";
    cases[n].icon0 = trailing;
    cases[n++].icon0_size = good_len + 1u;
    cases[n].what = "over the size cap";
    cases[n].icon0 = oversized;
    cases[n++].icon0_size = NK_STEAM_GRID_MAX_BYTES + 1u;
    cases[n].what = "under the smallest PNG";
    cases[n].icon0 = good;
    cases[n++].icon0_size = 30u;

    for (size_t i = 0u; i < n; ++i) {
        char leaf[64];
        char id[16];
        NkSteamGridArt art = {cases[i].icon0, cases[i].icon0_size, good, good_len};
        uint32_t app_id = nk_steam_shortcut_app_id(kGameA.exe, kGameA.app_name);
        snprintf(leaf, sizeof(leaf), "steam_art_bad_icon_%u", (unsigned)i);
        snprintf(id, sizeof(id), "3002%u", (unsigned)i);
        art_account(leaf, id, steam, sizeof(steam), vdf, sizeof(vdf));
        CHECK_STATUS(nk_steam_add_for_account_with_art(steam, id, &kGameA, is_running_no,
                                                       "20261115-110200", backup, sizeof(backup),
                                                       &ch, &art, NK_STEAM_GRID_KEEP_EXISTING,
                                                       &res),
                     NK_STEAM_OK, cases[i].what);
        vdf_bytes = read_bytes(vdf, &vdf_size);
        CHECK(ch == NK_STEAM_CHANGE_ADDED && vdf_bytes != NULL &&
                  count_bytes(vdf_bytes, vdf_size, kGameA.launch_options) == 1u,
              "the shortcut is added even though ICON0.PNG is unusable");
        free(vdf_bytes);
        join_rel_path(grid_dir, sizeof(grid_dir), steam, "userdata/");
        snprintf(grid_dir + strlen(grid_dir), sizeof(grid_dir) - strlen(grid_dir),
                 "%s/config/grid", id);
        grid_file_in(p_icon, sizeof(p_icon), grid_dir, app_id, "_icon.png");
        grid_file_in(p_hero, sizeof(p_hero), grid_dir, app_id, "_hero.png");
        CHECK(nk_platform_path_missing(p_icon) && file_is(p_hero, good, good_len),
              "the bad ICON0 is skipped and the good PIC1 is still used");
        CHECK(strstr(res.skipped, "ICON0.PNG") != NULL && strstr(res.skipped, "PIC1") == NULL,
              "the skipped picture is named, and only that one");
    }

    /* Neither picture usable: the shortcut alone, no grid folder at all. */
    {
        NkSteamGridArt art = {not_png, sizeof(not_png) - 1u, oversized,
                              NK_STEAM_GRID_MAX_BYTES + 1u};
        art_account("steam_art_bad_both", "30029", steam, sizeof(steam), vdf, sizeof(vdf));
        CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30029", &kGameA, is_running_no,
                                                       "20261115-110201", backup, sizeof(backup),
                                                       &ch, &art, NK_STEAM_GRID_KEEP_EXISTING,
                                                       &res),
                     NK_STEAM_OK, "two unusable pictures still add the shortcut");
        join_rel_path(grid_dir, sizeof(grid_dir), steam, "userdata/30029/config/grid");
        CHECK(ch == NK_STEAM_CHANGE_ADDED && res.written_mask == 0u &&
                  nk_platform_path_missing(grid_dir),
              "no picture is written and no grid folder is made");
        CHECK(strstr(res.skipped, "ICON0.PNG") != NULL && strstr(res.skipped, "PIC1.PNG") != NULL,
              "both skipped pictures are named");
    }
    free(oversized);
}

/* Steam takes a non-Steam shortcut's icon from the entry's "icon" field, so a new entry
 * names the copied icon there, and leaves it empty without one (#2630 review, finding 4). */
static void test_grid_icon_is_named_in_the_shortcut(void) {
    char steam[NK_MAX_PATH], vdf[NK_MAX_PATH * 2], backup[NK_MAX_PATH];
    char icon_path[NK_MAX_PATH * 2];
    uint8_t icon0[2048], pic1[2048];
    size_t icon0_len = make_synthetic_png(9u, 5u, 4u, 5u, 6u, 255u, icon0, sizeof(icon0));
    size_t pic1_len = make_synthetic_png(12u, 7u, 7u, 8u, 9u, 255u, pic1, sizeof(pic1));
    NkSteamGridResult res;
    NkSteamChange ch = NK_STEAM_CHANGE_NONE;
    uint8_t *bytes;
    size_t size = 0u;
    uint8_t want[NK_MAX_PATH * 2 + 8];
    size_t want_len;

    {
        NkSteamGridArt art = {icon0, icon0_len, pic1, pic1_len};
        art_account("steam_art_icon_field", "30030", steam, sizeof(steam), vdf, sizeof(vdf));
        CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30030", &kGameA, is_running_no,
                                                       "20261115-110300", backup, sizeof(backup),
                                                       &ch, &art, NK_STEAM_GRID_KEEP_EXISTING,
                                                       &res),
                     NK_STEAM_OK, "add with an icon");
        CHECK(nk_steam_grid_slot_path(res.grid_dir, res.app_id, NK_STEAM_GRID_SLOT_ICON,
                                      icon_path, sizeof(icon_path)),
              "icon path");
        CHECK(file_is(icon_path, icon0, icon0_len), "the icon file is there");
        memcpy(want, "icon\0", 5u);
        want_len = 5u + strlen(icon_path) + 1u;
        memcpy(want + 5u, icon_path, strlen(icon_path) + 1u);
        bytes = read_bytes(vdf, &size);
        CHECK(bytes != NULL && find_bytes(bytes, size, want, want_len) != NULL,
              "the shortcut's icon field names the copied icon");
        free(bytes);
    }
    {
        NkSteamGridArt art = {NULL, 0u, pic1, pic1_len};
        art_account("steam_art_no_icon_field", "30031", steam, sizeof(steam), vdf, sizeof(vdf));
        CHECK_STATUS(nk_steam_add_for_account_with_art(steam, "30031", &kGameA, is_running_no,
                                                       "20261115-110301", backup, sizeof(backup),
                                                       &ch, &art, NK_STEAM_GRID_KEEP_EXISTING,
                                                       &res),
                     NK_STEAM_OK, "add without an icon");
        bytes = read_bytes(vdf, &size);
        CHECK(bytes != NULL && find_bytes(bytes, size, "\x01icon\0\0", 7u) != NULL,
              "without ICON0.PNG the icon field stays empty");
        free(bytes);
    }
}

int main(void) {
    native_test_create_root("steam_shortcuts");
    snprintf(g_root, sizeof(g_root), "%s", native_test_get_root());
    test_app_id();
    test_new_file();
    test_existing_entries_are_preserved();
    test_three_adds_number_in_order();
    test_existing_executable_is_reported_not_added();
    test_no_shortcuts_map_gets_one();
    test_corrupt_files_are_refused();
    test_arguments();
    test_steam_running_fails_closed();
    test_file_add_and_backup();
    test_running_steam_blocks_every_write();
    test_corrupt_file_is_left_alone();
    test_a_failed_backup_writes_nothing();
    test_bad_stamp();
    test_accounts_and_add_for_account();
    test_each_game_is_its_own_shortcut();
    test_a_moved_folder_updates_its_own_entry();
    test_file_update_is_backed_up();
    test_account_names();
    test_grid_slot_paths_and_suffixes();
    test_grid_writes_and_reads_back();
    test_grid_steam_running_and_overwrite_guards();
    test_grid_invalid_pngs_and_rollback();
    test_add_for_account_with_art_and_rollback();
    test_grid_each_title_gets_its_own_app_id();
    test_grid_failed_restore_keeps_the_backup();
    test_grid_bad_art_never_blocks_the_shortcut();
    test_grid_icon_is_named_in_the_shortcut();
    if (g_failures) {
        fprintf(stderr, "test_steam_shortcuts: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_steam_shortcuts: ALL %d CHECKS PASSED\n", g_checks);
    return 0;
}
