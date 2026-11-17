// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The RetroAchievements bridge (src/core/nk_achievements.c, #1316) on the vendored rcheevos
 * rc_client, against an in-process fake server: no socket, no network (AGENTS.md section 5).
 * The fake answers in the response shapes rcheevos' parsers read (src/rapi/rc_api_user.c,
 * src/rapi/rc_api_runtime.c); the game, achievement and user are synthetic. */

#include "nk_achievements.h"
#include "nk_iso.h"

#include "../../third_party/rcheevos/include/rc_client.h"
#include "../../third_party/rcheevos/include/rc_consoles.h"
#include "../../third_party/rcheevos/src/rhash/md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...)                                                                     \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            g_failures++;                                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                             \
            fprintf(stderr, __VA_ARGS__);                                                    \
            fputc('\n', stderr);                                                             \
        }                                                                                    \
    } while (0)

#define USER_RAM NK_ACH_PSP_USER_RAM_START
#define USER_RAM_SIZE NK_ACH_PSP_USER_RAM_SIZE
#define HASH "0123456789abcdef0123456789abcdef"
#define ACH_ADDR 0x00100u           /* user RAM offset the synthetic achievement watches */

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

static void iso_put_both16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void iso_put_both32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
    out[4] = (uint8_t)(value >> 24);
    out[5] = (uint8_t)(value >> 16);
    out[6] = (uint8_t)(value >> 8);
    out[7] = (uint8_t)value;
}

static size_t iso_add_record(uint8_t *directory, size_t offset, const uint8_t *name,
                             uint8_t name_size, uint32_t lba, uint32_t size, bool is_dir) {
    uint8_t *record = directory + offset;
    const size_t record_size = 33u + name_size + (name_size % 2u == 0u ? 1u : 0u);
    memset(record, 0, record_size);
    record[0] = (uint8_t)record_size;
    iso_put_both32(record + 2u, lba);
    iso_put_both32(record + 10u, size);
    record[18] = 126u;
    record[19] = 1u;
    record[20] = 1u;
    record[25] = is_dir ? 2u : 0u;
    iso_put_both16(record + 28u, 1u);
    record[32] = name_size;
    memcpy(record + 33u, name, name_size);
    return offset + record_size;
}

static size_t iso_add_special_record(uint8_t *directory, size_t offset, uint8_t name,
                                    uint32_t lba) {
    return iso_add_record(directory, offset, &name, 1u, lba, 2048u, true);
}

static bool write_synthetic_psp_iso(char *path, size_t path_capacity,
                                    const uint8_t *sfo, size_t sfo_size,
                                    const uint8_t *eboot, size_t eboot_size) {
    enum { SECTOR = 2048, ISO_SECTORS = 25, ROOT_LBA = 20, PSP_LBA = 21,
           SYSDIR_LBA = 22, SFO_LBA = 23, EBOOT_LBA = 24 };
    static const uint8_t dot = 0u;
    static const uint8_t dotdot = 1u;
    static const uint8_t root_name[] = "PSP_GAME";
    static const uint8_t sfo_name[] = "PARAM.SFO;1";
    static const uint8_t sysdir_name[] = "SYSDIR";
    static const uint8_t eboot_name[] = "EBOOT.BIN;1";
    uint8_t disc[ISO_SECTORS * SECTOR];
    memset(disc, 0, sizeof disc);
    if (sfo_size == 0u || sfo_size > UINT32_MAX || eboot_size == 0u || eboot_size > UINT32_MAX)
        return false;

    uint8_t *pvd = disc + 16u * SECTOR;
    pvd[0] = 1u;
    memcpy(pvd + 1u, "CD001", 5u);
    pvd[6] = 1u;
    size_t pvd_record = iso_add_record(pvd, 156u, &dot, 1u, ROOT_LBA, SECTOR, true);
    CHECK(pvd_record == 190u, "PVD root record length %u", (unsigned)pvd_record);

    uint8_t *root = disc + ROOT_LBA * SECTOR;
    size_t at = 0u;
    at = iso_add_special_record(root, at, dot, ROOT_LBA);
    at = iso_add_special_record(root, at, dotdot, ROOT_LBA);
    at = iso_add_record(root, at, root_name, (uint8_t)(sizeof root_name - 1u),
                        PSP_LBA, SECTOR, true);

    uint8_t *psp = disc + PSP_LBA * SECTOR;
    at = 0u;
    at = iso_add_special_record(psp, at, dot, PSP_LBA);
    at = iso_add_special_record(psp, at, dotdot, ROOT_LBA);
    at = iso_add_record(psp, at, sfo_name, (uint8_t)(sizeof sfo_name - 1u),
                        SFO_LBA, (uint32_t)sfo_size, false);
    at = iso_add_record(psp, at, sysdir_name, (uint8_t)(sizeof sysdir_name - 1u),
                        SYSDIR_LBA, SECTOR, true);

    uint8_t *sysdir = disc + SYSDIR_LBA * SECTOR;
    at = 0u;
    at = iso_add_special_record(sysdir, at, dot, SYSDIR_LBA);
    at = iso_add_special_record(sysdir, at, dotdot, PSP_LBA);
    at = iso_add_record(sysdir, at, eboot_name, (uint8_t)(sizeof eboot_name - 1u),
                        EBOOT_LBA, (uint32_t)eboot_size, false);

    memcpy(disc + SFO_LBA * SECTOR, sfo, sfo_size);
    memcpy(disc + EBOOT_LBA * SECTOR, eboot, eboot_size);
#ifdef _WIN32
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    const int path_size = snprintf(path, path_capacity, "%s%c%s", SR_SELFTEST_BUILD_ROOT,
                                   separator, "ra-hash-selftest.iso");
    if (path_size < 0 || (size_t)path_size >= path_capacity) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    const bool wrote = fwrite(disc, 1u, sizeof disc, file) == sizeof disc;
    const bool closed = fclose(file) == 0;
    return wrote && closed;
}

/* ---- the fake server ------------------------------------------------------------------ */

typedef struct Fake {
    int requests;
    char log[16][512];       /* the post data of each request, in order */
    int award_status;        /* the HTTP status awardachievement answers */
    const char *award_body;  /* its body when not NULL, else k_award */
    const char *refuse_award; /* an award whose post data holds this is answered k_refused, 200 */
} Fake;

static const char k_login[] =
    "{\"Success\":true,\"User\":\"Player\",\"Token\":\"TOKEN\",\"Score\":0,"
    "\"SoftcoreScore\":12,\"Messages\":0}";
static const char k_sets[] =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
    "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
    "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"\",\"Achievements\":["
    "{\"ID\":5501,\"Title\":\"Five\",\"Description\":\"The watched byte reads 5\","
    "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800100=5\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00001\",\"Created\":1700000000,\"Modified\":1700000000},"
    "{\"ID\":5502,\"Title\":\"Kernel\",\"Description\":\"Reads memory outside the span\","
    "\"Flags\":3,\"Points\":1,\"MemAddr\":\"0xH000100=5\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00002\",\"Created\":1700000000,\"Modified\":1700000000}"
    "],\"Leaderboards\":[]}]}";
static const char k_session[] =
    "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1700000000}";
static const char k_award[] =
    "{\"Success\":true,\"Score\":0,\"SoftcoreScore\":17,\"AchievementID\":5501,"
    "\"AchievementsRemaining\":0}";
static const char k_refused[] = "{\"Success\":false,\"Error\":\"Unknown achievement\"}";
static const char k_bad_account[] =
    "{\"Success\":false,\"Error\":\"Invalid user/token combination.\"}";
static const char k_ok[] = "{\"Success\":true}";

static int starts_with(const char *s, const char *prefix) {
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

static void fake_transport(void *ctx, const NkAchRequest *req, NkAchDone done, void *done_ctx) {
    Fake *f = (Fake *)ctx;
    const char *post = req->post_data ? req->post_data : "";
    if (f->requests < 16) snprintf(f->log[f->requests], sizeof f->log[0], "%s", post);
    f->requests++;
    NkAchResponse r = {k_ok, sizeof k_ok - 1u, 200};
    if (starts_with(post, "r=login2&")) r.body = k_login;
    else if (starts_with(post, "r=achievementsets&")) r.body = k_sets;
    else if (starts_with(post, "r=startsession&")) r.body = k_session;
    else if (starts_with(post, "r=awardachievement&")) {
        r.body = f->award_body ? f->award_body : k_award;
        r.http_status = f->award_status;
        if (f->award_status < 0) r.body = "";
        if (f->refuse_award && strstr(post, f->refuse_award)) {
            r.body = k_refused;
            r.http_status = 200;
        }
    }
    r.body_length = strlen(r.body);
    done(&r, done_ctx);
}

static int count_requests(const Fake *f, const char *prefix) {
    int count = 0;
    for (int i = 0; i < f->requests && i < 16; i++)
        if (starts_with(f->log[i], prefix)) count++;
    return count;
}

static int find_request(const Fake *f, const char *prefix) {
    for (int i = 0; i < f->requests && i < 16; i++)
        if (starts_with(f->log[i], prefix)) return i;
    return -1;
}

static size_t take(NkAch *ach, NkAchEvent *ev, size_t max) { return nk_ach_take_events(ach, ev, max); }

static int has_event(const NkAchEvent *ev, size_t n, NkAchEventKind kind) {
    for (size_t i = 0; i < n; i++)
        if (ev[i].kind == kind) return (int)i;
    return -1;
}

/* ---- tests ------------------------------------------------------------------------------ */

static void test_url_gate(void) {
    CHECK(nk_ach_url_allowed("https://retroachievements.org/dorequest.php"), "RA server");
    static const char *refused[] = {
        "http://retroachievements.org/dorequest.php",
        "https://retroachievements.org.example.net/dorequest.php",
        "https://retroachievements.org:8443/dorequest.php",
        "https://user@retroachievements.org/dorequest.php",
        "https://example.net/?https://retroachievements.org/",
        "https://media.retroachievements.org/Badge/00001.png",
        "https://RETROACHIEVEMENTS.ORG/dorequest.php",
        "https://retroachievements.org",
        "",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        CHECK(!nk_ach_url_allowed(refused[i]), "refuses %s", refused[i]);
    CHECK(!nk_ach_url_allowed(NULL), "refuses NULL");
}

static void test_psp_disc_hash(void) {
    static const uint8_t sfo[] = "synthetic PARAM.SFO bytes";
    static const uint8_t eboot[] = "synthetic EBOOT.BIN bytes";
    char path[512] = "";
    char actual[33] = "";
    md5_state_t md5;
    md5_byte_t digest[16];
    static const char hex[] = "0123456789abcdef";
    char expected[33];
    CHECK(write_synthetic_psp_iso(path, sizeof path, sfo, sizeof sfo - 1u,
                                  eboot, sizeof eboot - 1u),
          "write the synthetic PSP ISO fixture");
    if (!path[0]) return;
    md5_init(&md5);
    md5_append(&md5, sfo, (int)(sizeof sfo - 1u));
    md5_append(&md5, eboot, (int)(sizeof eboot - 1u));
    md5_finish(&md5, digest);
    for (size_t i = 0; i < sizeof digest; i++) {
        expected[i * 2u] = hex[digest[i] >> 4];
        expected[i * 2u + 1u] = hex[digest[i] & 15u];
    }
    expected[32] = '\0';
    CHECK(nk_ach_psp_hash_iso(path, actual), "hash the synthetic PSP disc");
    CHECK(strcmp(actual, expected) == 0, "PSP hash order and MD5 (%s != %s)", actual, expected);
}

static void test_memory_map(uint8_t *ram) {
    /* The map the bridge relies on: RA 0..0x03FFFFFF is guest 0x08000000.. in one piece. */
    const rc_memory_regions_t *regions = rc_console_memory_regions(NK_ACH_CONSOLE_PSP);
    CHECK(regions && regions->num_regions == 3u, "the PSP map has three regions");
    if (regions) {
        uint32_t next = 0u;
        for (uint32_t i = 0; i < regions->num_regions; i++) {
            CHECK(regions->region[i].start_address == next, "region %u starts where the last ended", i);
            CHECK(regions->region[i].real_address == 0x08000000u + regions->region[i].start_address,
                  "region %u is guest 0x08000000 + its RA address", i);
            next = regions->region[i].end_address + 1u;
        }
        CHECK(next == 0x04000000u, "the map ends at RA 0x04000000");
    }

    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    uint8_t buf[8];
    ram[ACH_ADDR] = 0x5a;
    CHECK(nk_ach_read_memory(&mem, 0x00800000u + ACH_ADDR, buf, 1u) == 1u && buf[0] == 0x5a,
          "RA 0x00800100 is guest 0x08800100");
    /* Kernel RAM (RA below 0x00800000) and extended RAM are outside this span: nothing read. */
    CHECK(nk_ach_read_memory(&mem, 0x00000000u, buf, 4u) == 0u, "kernel RAM is outside the span");
    CHECK(nk_ach_read_memory(&mem, 0x007ffffcu, buf, 8u) == 0u, "a read starting below the span");
    CHECK(nk_ach_read_memory(&mem, 0x02000000u, buf, 4u) == 0u, "extended RAM is outside the span");
    CHECK(nk_ach_read_memory(&mem, 0x04000000u, buf, 4u) == 0u, "past the map");
    CHECK(nk_ach_read_memory(&mem, 0xfffffffcu, buf, 4u) == 0u, "the top of the address space");
    /* A read that runs off the end of the span stops there. */
    memset(buf, 0xee, sizeof buf);
    ram[USER_RAM_SIZE - 2u] = 1;
    ram[USER_RAM_SIZE - 1u] = 2;
    CHECK(nk_ach_read_memory(&mem, 0x00800000u + USER_RAM_SIZE - 2u, buf, 4u) == 2u &&
              buf[0] == 1 && buf[1] == 2 && buf[2] == 0xee,
          "a read at the end of the span stops at its last byte");
    /* A span shorter than the buffer behind it: a read past the span's end must read nothing even
     * though the bytes there exist. */
    const NkAchMemory small = {ram, USER_RAM, 0x1000u};
    CHECK(nk_ach_read_memory(&small, 0x00800000u + 0x1010u, buf, 4u) == 0u,
          "a read past the span's end reads nothing");
    CHECK(nk_ach_read_memory(&small, 0x00800000u + 0x0ffeu, buf, 4u) == 2u,
          "a read across the span's end stops at it");
    /* The regions are one run of guest memory: a read across RA 0x00800000 (kernel RAM into user
     * RAM) is whole when the span covers both sides. */
    const NkAchMemory across = {ram, 0x087ff000u, 0x2000u};
    ram[0xffeu] = 0x11;
    ram[0x1001u] = 0x44;
    CHECK(nk_ach_read_memory(&across, 0x007ffffeu, buf, 4u) == 4u && buf[0] == 0x11 && buf[3] == 0x44,
          "a read across a region boundary is whole");
    const NkAchMemory top = {ram, 0x0bfff000u, 0x2000u};
    CHECK(nk_ach_read_memory(&top, 0x03fffffeu, buf, 4u) == 2u, "a read stops at the end of the map");
    const NkAchMemory none = {NULL, USER_RAM, USER_RAM_SIZE};
    CHECK(nk_ach_read_memory(&none, 0x00800000u, buf, 1u) == 0u, "no memory, nothing read");
}

/* rc_client pointed at another host: the gate refuses every request before the transport. */
static void test_other_host_is_refused(uint8_t *ram) {
    static const char *hosts[] = {"https://example.net", "http://retroachievements.org",
                                  "https://retroachievements.org.example.net"};
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
        Fake fake;
        memset(&fake, 0, sizeof fake);
        const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
        NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
        CHECK(ach != NULL && nk_ach_rc_client(ach) != NULL, "create");
        if (!ach) return;
        rc_client_set_host(nk_ach_rc_client(ach), hosts[i]);
        CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
        NkAchEvent ev[8];
        const size_t n = take(ach, ev, 8);
        const int failed = has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED);
        CHECK(failed >= 0, "login to %s fails", hosts[i]);
        CHECK(fake.requests == 0, "nothing reached the transport for %s (%d requests)", hosts[i],
              fake.requests);
        if (failed >= 0)
            CHECK(strstr(ev[failed].message, "not a RetroAchievements server") != NULL,
                  "the gate refused %s: %s", hosts[i], ev[failed].message);
        CHECK(!nk_ach_logged_in(ach), "not logged in");
        nk_ach_destroy(ach);
    }
    CHECK(nk_ach_rc_client(NULL) == NULL, "no client without a bridge");
}

/* No transport: rc_client's requests are refused before anything is sent. */
static void test_no_transport_sends_nothing(uint8_t *ram) {
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, NULL, NULL);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    CHECK(!nk_ach_hardcore(ach), "softcore");
    CHECK(!nk_ach_begin_login(ach, "", "TOKEN"), "an empty name is refused");
    CHECK(!nk_ach_begin_login(ach, "Player", NULL), "a missing token is refused");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    NkAchEvent ev[8];
    const size_t n = take(ach, ev, 8);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED) >= 0, "login fails with no transport");
    CHECK(!nk_ach_logged_in(ach), "not logged in");
    nk_ach_destroy(ach);
}

static void test_login_load_unlock(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];

    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int login = has_event(ev, n, NK_ACH_EVENT_LOGIN_OK);
    CHECK(login >= 0, "login succeeds");
    if (login >= 0) CHECK(strcmp(ev[login].title, "Player") == 0, "display name %s", ev[login].title);
    CHECK(nk_ach_logged_in(ach), "logged in");
    const int lr = find_request(&fake, "r=login2&");
    CHECK(lr >= 0 && strstr(fake.log[lr], "u=Player") && strstr(fake.log[lr], "t=TOKEN") &&
              !strstr(fake.log[lr], "p="),
          "login sends the name and token, never a password: %s", lr >= 0 ? fake.log[lr] : "");

    CHECK(!nk_ach_begin_load_game(ach, "0123456789ABCDEF0123456789ABCDEF"), "upper-case hash");
    CHECK(!nk_ach_begin_load_game(ach, "0123"), "short hash");
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int loaded = has_event(ev, n, NK_ACH_EVENT_GAME_LOADED);
    CHECK(loaded >= 0, "game loads");
    if (loaded >= 0)
        CHECK(ev[loaded].id == 1234u && ev[loaded].count == 2u && ev[loaded].points == 0u,
              "game 1234 with two achievements, counted apart from points (id %u, count %u, "
              "points %u)", ev[loaded].id, ev[loaded].count, ev[loaded].points);
    CHECK(nk_ach_game_loaded(ach), "game loaded");
    NkAchAchievement list[4];
    size_t achievement_total = 0u;
    const size_t achievement_count = nk_ach_copy_achievement_list(
        ach, list, sizeof list / sizeof list[0], &achievement_total);
    CHECK(achievement_count == 2u && achievement_total == 2u,
          "the player can copy the core achievement list (%u copied, %u total)",
          (unsigned)achievement_count, (unsigned)achievement_total);
    bool found_five = false;
    for (size_t i = 0u; i < achievement_count; i++)
        if (strcmp(list[i].title, "Five") == 0 && list[i].points == 5u) found_five = true;
    CHECK(found_five, "the core list includes the synthetic achievement title and points");
    /* 5502 reads kernel RAM, outside the span. rc_client reads every address once when it
     * activates the set, turns 5502 off, and the user is told, before GAME_LOADED. */
    const int disabled = has_event(ev, n, NK_ACH_EVENT_DISABLED);
    CHECK(disabled >= 0 && disabled < loaded, "an achievement that cannot read its memory is reported");
    if (disabled >= 0)
        CHECK(ev[disabled].id == 5502u && strstr(ev[disabled].message, "Invalid address") != NULL,
              "DISABLED names 5502 (got %u: %s)", ev[disabled].id, ev[disabled].message);
    const int sets = find_request(&fake, "r=achievementsets&");
    CHECK(sets >= 0 && strstr(fake.log[sets], "m=" HASH), "the hash identifies the game");
    const int session = find_request(&fake, "r=startsession&");
    CHECK(session >= 0 && strstr(fake.log[session], "&h=0"), "the session is softcore: %s",
          session >= 0 ? fake.log[session] : "");

    /* The achievement must be seen false before it can trigger. */
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKED) < 0, "no unlock while the byte reads 0");
    CHECK(has_event(ev, n, NK_ACH_EVENT_DISABLED) < 0, "an achievement is reported off once");
    CHECK(find_request(&fake, "r=awardachievement&") < 0, "no award request yet");

    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int unlocked = has_event(ev, n, NK_ACH_EVENT_UNLOCKED);
    CHECK(unlocked >= 0, "the achievement unlocks when guest 0x08800100 reads 5");
    if (unlocked >= 0)
        CHECK(ev[unlocked].id == 5501u && ev[unlocked].points == 5u &&
                  strcmp(ev[unlocked].title, "Five") == 0,
              "unlock 5501 Five 5 (got %u %s %u)", ev[unlocked].id, ev[unlocked].title,
              ev[unlocked].points);
    const int award = find_request(&fake, "r=awardachievement&");
    CHECK(award >= 0 && strstr(fake.log[award], "a=5501") && strstr(fake.log[award], "&h=0"),
          "a softcore award for 5501 is sent: %s", award >= 0 ? fake.log[award] : "");
    CHECK(!nk_ach_hardcore(ach), "still softcore");
    nk_ach_destroy(ach);
}

/* An award that cannot reach the server stays pending: rc_client reports a disconnect. */
static void test_award_offline_is_pending(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = NK_ACH_HTTP_RETRY;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    nk_ach_begin_login(ach, "Player", "TOKEN");
    nk_ach_begin_load_game(ach, HASH);
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKED) >= 0, "the unlock is still shown");
    CHECK(has_event(ev, n, NK_ACH_EVENT_DISCONNECTED) >= 0, "the award is pending");
    CHECK(has_event(ev, n, NK_ACH_EVENT_SERVER_ERROR) < 0, "a retry is not a server error");

    NkAchPendingUnlock queued;
    memset(&queued, 0, sizeof queued);
    CHECK(nk_ach_pending_unlock_count(ach) == 1u, "the unlock is in the token-free queue");
    CHECK(nk_ach_copy_pending_unlocks(ach, &queued, 1u) == 1u,
          "copy the owner-persistable unlock record");
    CHECK(queued.achievement_id == 5501u && strcmp(queued.game_hash, HASH) == 0,
          "the queue holds the game hash and achievement id, not a request body");
    nk_ach_destroy(ach);

    Fake online;
    memset(&online, 0, sizeof online);
    online.award_status = 200;
    NkAch *restored = nk_ach_create(&mem, fake_transport, &online);
    CHECK(restored != NULL, "create the restored client");
    if (restored) {
        CHECK(nk_ach_restore_pending_unlock(restored, &queued),
              "restore the owner-persisted unlock record");
        CHECK(nk_ach_begin_login(restored, "Player", "TOKEN"), "restored login starts");
        nk_ach_replay_pending_unlocks(restored, 1000u);
        CHECK(find_request(&online, "r=awardachievement&") >= 0,
              "the restored unlock is submitted through rcheevos' request builder");
        const int award = find_request(&online, "r=awardachievement&");
        CHECK(award >= 0 && strstr(online.log[award], "t=TOKEN") &&
                  strstr(online.log[award], "a=5501") && strstr(online.log[award], "m=" HASH),
              "the replay uses the current OS token and the original game hash");
        CHECK(nk_ach_pending_unlock_count(restored) == 0u,
              "a successful replay removes the stored record");
        NkAchEvent restored_events[NK_ACH_EVENT_QUEUE];
        const size_t restored_count = take(restored, restored_events, NK_ACH_EVENT_QUEUE);
        CHECK(has_event(restored_events, restored_count, NK_ACH_EVENT_RECONNECTED) >= 0,
              "the player is told when the persisted queue is synchronized");
        nk_ach_destroy(restored);
    }
}

static NkAchPendingUnlock saved_unlock(uint32_t id) {
    NkAchPendingUnlock unlock;
    memset(&unlock, 0, sizeof unlock);
    snprintf(unlock.game_hash, sizeof unlock.game_hash, "%s", HASH);
    unlock.achievement_id = id;
    unlock.unlocked_at_utc = 1700000000u;
    return unlock;
}

/* Sign out, then another account signs in: a saved record names no account, so it must not be
 * awarded to the new one. */
static void test_sign_out_discards_saved_unlocks(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    const NkAchPendingUnlock saved = saved_unlock(5501u);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(nk_ach_restore_pending_unlock(ach, &saved), "restore the first account's record");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "the first account signs in");
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(nk_ach_sign_out(ach) == 1u, "sign-out reports the one record it discarded");
    CHECK(!nk_ach_logged_in(ach), "signed out");
    CHECK(nk_ach_pending_unlock_count(ach) == 0u, "sign-out leaves no saved unlock");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_QUEUE_CHANGED) >= 0,
          "the player is told to rewrite the queue file");
    CHECK(nk_ach_begin_login(ach, "Other", "TOKEN2"), "another account signs in");
    nk_ach_replay_pending_unlocks(ach, 1000u);
    nk_ach_replay_pending_unlocks(ach, 40000u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 0,
          "the first account's unlock is not awarded to the next account (%d award requests)",
          count_requests(&fake, "r=awardachievement&"));
    CHECK(nk_ach_sign_out(ach) == 0u, "nothing left to discard");
    CHECK(nk_ach_sign_out(NULL) == 0u, "no bridge, nothing discarded");
    nk_ach_destroy(ach);

    /* The switch turned off (nk_ach_logout) keeps the records: the stored credential stays. */
    NkAch *kept = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(kept != NULL, "create");
    if (!kept) return;
    CHECK(nk_ach_restore_pending_unlock(kept, &saved), "restore");
    nk_ach_logout(kept);
    CHECK(nk_ach_pending_unlock_count(kept) == 1u, "logout keeps the saved unlock");
    nk_ach_destroy(kept);
}

/* An unlock the server refuses by its own error leaves the queue and is reported; the records
 * behind it are still sent. */
static void test_refused_saved_unlock_does_not_hold_the_queue(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.refuse_award = "a=7001&";
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    const NkAchPendingUnlock refused = saved_unlock(7001u);
    const NkAchPendingUnlock good = saved_unlock(5501u);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(nk_ach_restore_pending_unlock(ach, &refused) &&
              nk_ach_restore_pending_unlock(ach, &good), "restore two records");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);

    nk_ach_replay_pending_unlocks(ach, 1000u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 1, "the first record is sent");
    CHECK(nk_ach_pending_unlock_count(ach) == 1u,
          "the refused record left the queue (%u left)",
          (unsigned)nk_ach_pending_unlock_count(ach));
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int told = has_event(ev, n, NK_ACH_EVENT_UNLOCK_REFUSED);
    CHECK(told >= 0, "the refusal is reported");
    if (told >= 0)
        CHECK(ev[told].id == 7001u && strcmp(ev[told].message, "Unknown achievement") == 0,
              "UNLOCK_REFUSED names 7001 and the server's error (got %u: %s)", ev[told].id,
              ev[told].message);
    CHECK(has_event(ev, n, NK_ACH_EVENT_QUEUE_CHANGED) >= 0, "the queue file is rewritten");

    nk_ach_replay_pending_unlocks(ach, 1001u);
    const int award = count_requests(&fake, "r=awardachievement&");
    CHECK(award == 2 && strstr(fake.log[fake.requests - 1], "a=5501&") != NULL,
          "the record behind it is sent next, with no 30 s wait (%d award requests)", award);
    CHECK(nk_ach_pending_unlock_count(ach) == 0u, "the queue is empty");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_RECONNECTED) >= 0, "the player is told the queue is sent");
    nk_ach_replay_pending_unlocks(ach, 40000u);
    nk_ach_replay_pending_unlocks(ach, 80000u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 2,
          "the refused unlock is never sent again (%d award requests)",
          count_requests(&fake, "r=awardachievement&"));
    nk_ach_destroy(ach);
}

/* No definite answer: the record waits 30 seconds. A refused account: every record waits for
 * the next sign-in, and nothing is sent until then. */
static void test_saved_unlock_waits_for_the_server_and_the_account(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 503;
    fake.award_body = "";
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    const NkAchPendingUnlock saved = saved_unlock(5501u);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(nk_ach_restore_pending_unlock(ach, &saved), "restore");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    nk_ach_replay_pending_unlocks(ach, 1000u);
    nk_ach_replay_pending_unlocks(ach, 30999u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 1 &&
              nk_ach_pending_unlock_count(ach) == 1u,
          "a 503 keeps the record and is not retried inside 30 s");
    nk_ach_replay_pending_unlocks(ach, 31000u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 2, "it is retried after 30 s");

    fake.award_status = 401;
    fake.award_body = k_bad_account;
    nk_ach_replay_pending_unlocks(ach, 70000u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 3 &&
              nk_ach_pending_unlock_count(ach) == 1u,
          "a refused account keeps the record");
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    nk_ach_replay_pending_unlocks(ach, 110000u);
    nk_ach_replay_pending_unlocks(ach, 150000u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 3,
          "nothing more is sent until the next sign-in (%d award requests)",
          count_requests(&fake, "r=awardachievement&"));
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCK_REFUSED) < 0, "the record was not dropped");

    fake.award_status = 200;
    fake.award_body = NULL;
    nk_ach_logout(ach);
    CHECK(nk_ach_pending_unlock_count(ach) == 1u, "ending the session keeps the record");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "the next sign-in");
    nk_ach_replay_pending_unlocks(ach, 150001u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 4 &&
              nk_ach_pending_unlock_count(ach) == 0u,
          "the record is sent after the next sign-in");
    nk_ach_destroy(ach);
}

int main(void) {
    uint8_t *ram = (uint8_t *)calloc(1u, USER_RAM_SIZE);
    if (!ram) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    test_url_gate();
    test_psp_disc_hash();
    test_memory_map(ram);
    test_other_host_is_refused(ram);
    test_no_transport_sends_nothing(ram);
    test_login_load_unlock(ram);
    test_award_offline_is_pending(ram);
    test_sign_out_discards_saved_unlocks(ram);
    test_refused_saved_unlock_does_not_hold_the_queue(ram);
    test_saved_unlock_waits_for_the_server_and_the_account(ram);
    free(ram);
    if (g_failures) {
        fprintf(stderr, "test_nk_achievements: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_achievements: PASS\n");
    return 0;
}
