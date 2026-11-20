// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The RetroAchievements bridge (src/core/nk_achievements.c, #1316) on the vendored rcheevos
 * rc_client, against an in-process fake server: no socket, no network (AGENTS.md section 5).
 * The fake answers in the response shapes rcheevos' parsers read (src/rapi/rc_api_user.c,
 * src/rapi/rc_api_runtime.c); the game, achievement and user are synthetic. */

#include "nk_achievements.h"
#include "nk_iso.h"
#include "nk_platform.h"

#include "rc_api_info.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_version.h"

/* rhash's MD5, which signs a progress state's DONE chunk; vendored, not a public header. */
#include "../../third_party/rcheevos/src/rhash/md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#endif

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
    const char *login_user;  /* the account the login answer names when not NULL, else Player */
    int login_status;        /* the HTTP status login2 answers when not 0, else 200 */
    const char *login_body;  /* its body when not NULL */
    const char *sets_body;   /* the achievement sets answered when not NULL, else k_sets */
    char login_answer[256];
    /* A transport that answers later, from wherever the test calls the kept done. */
    bool defer;
    NkAchDone deferred_done;
    void *deferred_ctx;
    /* With defer, a transport with one slot, as the player's (src/player/main.c,
     * player_ach_transport): a request that arrives while one is held is answered
     * NK_ACH_HTTP_RETRY at once and never sent. */
    bool one_slot;
    int slot_refusals;
    /* The lock the bridge was given, when the test counts it (lock_enter, lock_leave). */
    int lock_depth;
    int lock_enters;
    int transport_calls_outside_lock;
    bool counts_lock;
} Fake;

static void lock_enter(void *context) {
    Fake *f = (Fake *)context;
    f->lock_depth++;
    f->lock_enters++;
}

static void lock_leave(void *context) { ((Fake *)context)->lock_depth--; }

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
/* A set whose second achievement has a trigger rcheevos cannot parse ("X:" is no condition
 * type), and the same set as the server's answer for another console (12). */
static const char k_sets_parse_error[] =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
    "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
    "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"\",\"Achievements\":["
    "{\"ID\":5501,\"Title\":\"Five\",\"Description\":\"The watched byte reads 5\","
    "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800100=5\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00001\",\"Created\":1700000000,\"Modified\":1700000000},"
    "{\"ID\":5503,\"Title\":\"Unparsable\",\"Description\":\"A trigger with no valid form\","
    "\"Flags\":3,\"Points\":1,\"MemAddr\":\"X:0xH800100=5\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00003\",\"Created\":1700000000,\"Modified\":1700000000}"
    "],\"Leaderboards\":[]}]}";
static const char k_sets_other_console[] =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":12,"
    "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
    "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"\",\"Achievements\":["
    "{\"ID\":5501,\"Title\":\"Five\",\"Description\":\"The watched byte reads 5\","
    "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800100=5\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00001\",\"Created\":1700000000,\"Modified\":1700000000}"
    "],\"Leaderboards\":[]}]}";

static int starts_with(const char *s, const char *prefix) {
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

/* What the server answers post when the fake answers it. */
static NkAchResponse fake_response(Fake *f, const char *post) {
    NkAchResponse r = {k_ok, sizeof k_ok - 1u, 200};
    if (starts_with(post, "r=login2&")) {
        r.body = k_login;
        if (f->login_user) {
            snprintf(f->login_answer, sizeof f->login_answer,
                     "{\"Success\":true,\"User\":\"%s\",\"Token\":\"TOKEN\",\"Score\":0,"
                     "\"SoftcoreScore\":12,\"Messages\":0}", f->login_user);
            r.body = f->login_answer;
        }
        if (f->login_body) r.body = f->login_body;
        if (f->login_status) r.http_status = f->login_status;
        if (f->login_status < 0) r.body = "";
    }
    else if (starts_with(post, "r=achievementsets&")) r.body = f->sets_body ? f->sets_body : k_sets;
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
    return r;
}

static void fake_transport(void *ctx, const NkAchRequest *req, NkAchDone done, void *done_ctx) {
    Fake *f = (Fake *)ctx;
    const char *post = req->post_data ? req->post_data : "";
    if (f->defer && f->one_slot && f->deferred_done) {
        const NkAchResponse busy = {NULL, 0u, NK_ACH_HTTP_RETRY};
        f->slot_refusals++;
        done(&busy, done_ctx);
        return;
    }
    if (f->requests < 16) snprintf(f->log[f->requests], sizeof f->log[0], "%s", post);
    f->requests++;
    if (f->counts_lock && f->lock_depth <= 0) f->transport_calls_outside_lock++;
    if (f->defer) {
        f->deferred_done = done;
        f->deferred_ctx = done_ctx;
        return;
    }
    const NkAchResponse r = fake_response(f, post);
    done(&r, done_ctx);
}

/* Answers the request the deferring transport holds, as it would have at once, and frees its
 * slot first, as the player's poll does. False when none is held. */
static bool fake_answer_held(Fake *f) {
    NkAchDone done = f->deferred_done;
    void *done_ctx = f->deferred_ctx;
    if (!done || f->requests < 1 || f->requests > 16) return false;
    f->deferred_done = NULL;
    f->deferred_ctx = NULL;
    const NkAchResponse r = fake_response(f, f->log[f->requests - 1]);
    done(&r, done_ctx);
    return true;
}

static int count_requests(const Fake *f, const char *prefix) {
    int count = 0;
    for (int i = 0; i < f->requests && i < 16; i++)
        if (starts_with(f->log[i], prefix)) count++;
    return count;
}

/* The fake server's answer to one request's post data, logged in order. */
static NkAchResponse fake_answer(Fake *f, const char *post_data) {
    const char *post = post_data ? post_data : "";
    if (f->requests < 16) snprintf(f->log[f->requests], sizeof f->log[0], "%s", post);
    f->requests++;
    return fake_response(f, post);
}

/* A transport that reached no server and says nothing more: done(NULL). */
static void silent_transport(void *ctx, const NkAchRequest *req, NkAchDone done, void *done_ctx) {
    (void)ctx;
    (void)req;
    done(NULL, done_ctx);
}

/* An asynchronous transport written to the header's contract: it keeps the request it was handed,
 * as given, and answers later, from the caller's loop (held_pump). Unlike Deferred below, which
 * copies the post data, it holds the request's own pointers, so a test sees whether they outlive
 * the call, and it answers as the fake server does. */
typedef struct Held {
    Fake fake;
    struct {
        const char *url;
        const char *post_data;
        NkAchDone done;
        void *done_ctx;
    } q[16];
    int queued;
    int answered;
} Held;

static void held_transport(void *ctx, const NkAchRequest *req, NkAchDone done, void *done_ctx) {
    Held *d = (Held *)ctx;
    if (d->queued >= 16) {
        done(NULL, done_ctx);
        return;
    }
    d->q[d->queued].url = req->url;
    d->q[d->queued].post_data = req->post_data;
    d->q[d->queued].done = done;
    d->q[d->queued].done_ctx = done_ctx;
    d->queued++;
}

/* Answers one waiting request, the oldest; false when none waits. */
static int held_answer_one(Held *d) {
    if (d->answered >= d->queued) return 0;
    const int i = d->answered++;
    NkAchResponse r = fake_answer(&d->fake, d->q[i].post_data);
    d->q[i].done(&r, d->q[i].done_ctx);
    return 1;
}

static void held_pump(Held *d) {
    while (held_answer_one(d)) {
    }
}

/* Overwrites the stack below the caller, where a frame that has returned kept its locals. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void scribble_stack(void) {
    volatile unsigned char junk[32768];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = 0xa5u;
}

static int has_prefix_n(const char *s, const char *prefix) {
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

/* A transport that answers later, as NkAchTransport allows ("now or later"): each request waits
 * until the test delivers an answer to it. */
typedef struct Pending {
    NkAchDone done;
    void *done_ctx;
    char post[512];
} Pending;

typedef struct Deferred {
    int count;
    Pending q[8];
} Deferred;

static void deferred_transport(void *ctx, const NkAchRequest *req, NkAchDone done, void *done_ctx) {
    Deferred *d = (Deferred *)ctx;
    if (d->count == 8) {
        NkAchResponse r = {"", 0u, NK_ACH_HTTP_CLIENT_ERROR};
        done(&r, done_ctx);
        return;
    }
    Pending *p = &d->q[d->count++];
    p->done = done;
    p->done_ctx = done_ctx;
    snprintf(p->post, sizeof p->post, "%s", req->post_data ? req->post_data : "");
}

/* Answers the oldest waiting request whose post data starts with prefix with body_length bytes of
 * body. False when none waits. */
static int deliver_n(Deferred *d, const char *prefix, int http_status, const char *body,
                     size_t body_length) {
    for (int i = 0; i < d->count; i++) {
        if (!starts_with(d->q[i].post, prefix)) continue;
        const Pending p = d->q[i];
        memmove(&d->q[i], &d->q[i + 1], (size_t)(d->count - i - 1) * sizeof d->q[0]);
        d->count--;
        NkAchResponse r = {body, body_length, http_status};
        p.done(&r, p.done_ctx);
        return 1;
    }
    return 0;
}

static int deliver(Deferred *d, const char *prefix, int http_status, const char *body) {
    return deliver_n(d, prefix, http_status, body, strlen(body));
}

/* Answers every waiting request as a client error, so nothing is left in flight. */
static void drain(Deferred *d) {
    while (d->count > 0) {
        const Pending p = d->q[0];
        memmove(&d->q[0], &d->q[1], (size_t)(d->count - 1) * sizeof d->q[0]);
        d->count--;
        NkAchResponse r = {"", 0u, NK_ACH_HTTP_CLIENT_ERROR};
        p.done(&r, p.done_ctx);
    }
}

static int count_events(const NkAchEvent *ev, size_t n, NkAchEventKind kind) {
    int c = 0;
    for (size_t i = 0; i < n; i++)
        if (ev[i].kind == kind) c++;
    return c;
}

static char g_log_award[160];
static void RC_CCONV award_log(const char *message, const rc_client_t *client) {
    (void)client;
    if (message && starts_with(message, "Awarded achievement "))
        snprintf(g_log_award, sizeof g_log_award, "%s", message);
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

/* The tag of the fake server's account, as a queue file names it. */
static const char *player_tag(void) {
    static char tag[NK_ACH_ACCOUNT_TAG_SIZE];
    if (!tag[0]) (void)nk_ach_account_tag("Player", tag);
    return tag;
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
        CHECK(nk_ach_restore_pending_unlock(restored, &queued, player_tag()),
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

/* Sign out, then another account signs in: the queue was the first account's, so nothing of it
 * is awarded to the new one. */
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
    CHECK(nk_ach_restore_pending_unlock(ach, &saved, player_tag()),
          "restore the first account's record");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "the first account signs in");
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(nk_ach_sign_out(ach) == 1u, "sign-out reports the one record it discarded");
    CHECK(!nk_ach_logged_in(ach), "signed out");
    CHECK(nk_ach_pending_unlock_count(ach) == 0u, "sign-out leaves no saved unlock");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_QUEUE_CHANGED) >= 0,
          "the player is told to rewrite the queue file");
    fake.login_user = "Other";
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
    CHECK(nk_ach_restore_pending_unlock(kept, &saved, player_tag()), "restore");
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
    CHECK(nk_ach_restore_pending_unlock(ach, &refused, player_tag()) &&
              nk_ach_restore_pending_unlock(ach, &good, player_tag()), "restore two records");
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
    CHECK(nk_ach_restore_pending_unlock(ach, &saved, player_tag()), "restore");
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

/* ---- the queue's owner ---------------------------------------------------------------- */

/* A queue belongs to one account. Records another account left are never sent with this
 * account's token: they are discarded, and the player is told how many. */
static void test_saved_unlocks_go_only_to_their_account(uint8_t *ram) {
    char player[NK_ACH_ACCOUNT_TAG_SIZE], lower[NK_ACH_ACCOUNT_TAG_SIZE];
    char other[NK_ACH_ACCOUNT_TAG_SIZE], none[NK_ACH_ACCOUNT_TAG_SIZE];
    CHECK(nk_ach_account_tag("Player", player) && nk_ach_account_tag("pLAYER", lower) &&
              nk_ach_account_tag("Other", other), "account tags");
    CHECK(strlen(player) == 32u && strcmp(player, lower) == 0,
          "one account, whatever the case of its name (%s, %s)", player, lower);
    CHECK(strcmp(player, other) != 0, "two accounts, two tags");
    CHECK(strstr(player, "layer") == NULL, "the tag does not spell the name");
    CHECK(!nk_ach_account_tag("", none) && !nk_ach_account_tag(NULL, none), "no name, no tag");

    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.login_user = "Other";
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    const NkAchPendingUnlock first = saved_unlock(5501u);
    const NkAchPendingUnlock second = saved_unlock(5502u);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    char owner[NK_ACH_ACCOUNT_TAG_SIZE];
    CHECK(!nk_ach_copy_pending_owner(ach, owner), "an empty queue has no owner");
    CHECK(!nk_ach_restore_pending_unlock(ach, &first, NULL) &&
              !nk_ach_restore_pending_unlock(ach, &first, "Player") &&
              nk_ach_pending_unlock_count(ach) == 0u, "a record needs its owner's tag");
    CHECK(nk_ach_restore_pending_unlock(ach, &first, player), "restore Player's record");
    CHECK(!nk_ach_restore_pending_unlock(ach, &second, other) &&
              nk_ach_pending_unlock_count(ach) == 1u,
          "another account's record does not join Player's queue");
    CHECK(nk_ach_restore_pending_unlock(ach, &second, player), "restore Player's second record");
    CHECK(nk_ach_copy_pending_owner(ach, owner) && strcmp(owner, player) == 0,
          "the queue is Player's");

    /* Other signs in with Player's records still saved (no Sign out came between). */
    CHECK(nk_ach_begin_login(ach, "Other", "TOKEN2"), "Other signs in");
    nk_ach_replay_pending_unlocks(ach, 1000u);
    nk_ach_replay_pending_unlocks(ach, 40000u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 0,
          "Player's unlocks are not sent with Other's token (%d award requests)",
          count_requests(&fake, "r=awardachievement&"));
    CHECK(nk_ach_pending_unlock_count(ach) == 0u, "they left the queue");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int told = has_event(ev, n, NK_ACH_EVENT_UNLOCKS_DISCARDED);
    CHECK(told >= 0 && has_event(ev, n, NK_ACH_EVENT_LOGIN_OK) >= 0 &&
              has_event(ev, n, NK_ACH_EVENT_QUEUE_CHANGED) >= 0,
          "the player is told, and told to rewrite the file");
    if (told >= 0) CHECK(ev[told].count == 2u, "two records discarded (got %u)", ev[told].count);

    /* A record restored while Other is signed in fares the same. */
    CHECK(nk_ach_restore_pending_unlock(ach, &first, player), "a late restore is accepted");
    nk_ach_replay_pending_unlocks(ach, 80000u);
    CHECK(count_requests(&fake, "r=awardachievement&") == 0 &&
              nk_ach_pending_unlock_count(ach) == 0u, "and discarded, unsent");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKS_DISCARDED) >= 0, "and reported");
    nk_ach_destroy(ach);
}

/* ---- the queue file ------------------------------------------------------------------- */

static void queue_path(char *path, size_t path_size, const char *name) {
#ifdef _WIN32
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    snprintf(path, path_size, "%s%c%s", SR_SELFTEST_BUILD_ROOT, separator, name);
}

static bool write_bytes(const char *path, const uint8_t *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    const bool wrote = fwrite(bytes, 1u, size, file) == size;
    return fclose(file) == 0 && wrote;
}

static size_t read_bytes(const char *path, uint8_t *out, size_t max) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0u;
    const size_t size = fread(out, 1u, max, file);
    fclose(file);
    return size;
}

static void put_le(uint8_t *out, uint64_t value, unsigned bytes) {
    for (unsigned i = 0u; i < bytes; i++) out[i] = (uint8_t)(value >> (8u * i));
}

/* A version 1 file, as the first, unreleased builds of this feature wrote it: no owner. */
static size_t old_queue_file(uint8_t *out, const NkAchPendingUnlock *records, uint32_t count) {
    memcpy(out, "NKRAQ001", 8u);
    put_le(out + 8u, 1u, 4u);
    put_le(out + 12u, count, 4u);
    for (uint32_t i = 0u; i < count; i++) {
        uint8_t *record = out + 16u + i * 44u;
        memcpy(record, records[i].game_hash, 32u);
        put_le(record + 32u, records[i].achievement_id, 4u);
        put_le(record + 36u, records[i].unlocked_at_utc, 8u);
    }
    return 16u + (size_t)count * 44u;
}

/* A version 2 file of Player's, in the format nk_achievements.h gives. */
static size_t queue_file(uint8_t *out, const NkAchPendingUnlock *records, uint32_t count) {
    memcpy(out, "NKRAQ001", 8u);
    put_le(out + 8u, 2u, 4u);
    put_le(out + 12u, count, 4u);
    memcpy(out + 16u, player_tag(), 32u);
    for (uint32_t i = 0u; i < count; i++) {
        uint8_t *record = out + 48u + i * 44u;
        memcpy(record, records[i].game_hash, 32u);
        put_le(record + 32u, records[i].achievement_id, 4u);
        put_le(record + 36u, records[i].unlocked_at_utc, 8u);
    }
    return 48u + (size_t)count * 44u;
}

static void remove_queue_files(const char *path) {
    char name[NK_MAX_PATH];
    (void)nk_remove_utf8(path);
    snprintf(name, sizeof name, "%s.tmp", path);
    (void)nk_remove_utf8(name);
    snprintf(name, sizeof name, "%s.unreadable", path);
    (void)nk_remove_utf8(name);
    for (unsigned n = 2u; n <= 99u; n++) {
        snprintf(name, sizeof name, "%s.unreadable.%u", path, n);
        (void)nk_remove_utf8(name);
    }
}

/* What one session saves, the next one loads, with its owner; only the owner sends it. */
static void test_queue_file_names_its_account(uint8_t *ram) {
    char path[NK_MAX_PATH];
    queue_path(path, sizeof path, "ra-queue-selftest.bin");
    remove_queue_files(path);
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAchQueueLoad report;
    uint8_t bytes[256];

    NkAch *writer = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(writer != NULL, "create");
    if (!writer) return;
    CHECK(nk_ach_load_queue_file(writer, path, &report) && report.loaded == 0u &&
              !report.unreadable, "no file is an empty queue, not an error");
    CHECK(nk_ach_save_queue_file(writer, path), "save an empty queue");
    CHECK(read_bytes(path, bytes, sizeof bytes) == 48u && bytes[8] == 2u && bytes[12] == 0u &&
              memcmp(bytes + 16u, "00000000000000000000000000000000", 32u) == 0,
          "an empty queue file: version 2, no record, no owner");
    const NkAchPendingUnlock saved = saved_unlock(5501u);
    CHECK(nk_ach_restore_pending_unlock(writer, &saved, player_tag()), "queue one record");
    CHECK(nk_ach_save_queue_file(writer, path), "save it");
    nk_ach_destroy(writer);
    CHECK(read_bytes(path, bytes, sizeof bytes) == 92u && memcmp(bytes, "NKRAQ001", 8u) == 0 &&
              bytes[8] == 2u && bytes[12] == 1u && memcmp(bytes + 16u, player_tag(), 32u) == 0 &&
              memcmp(bytes + 48u, HASH, 32u) == 0 && bytes[80] == 0x7du && bytes[81] == 0x15u,
          "the file: header, the owner's tag, then the record");
    for (size_t i = 0u; i + 6u <= 92u; i++)
        CHECK(memcmp(bytes + i, "Player", 6u) != 0 && memcmp(bytes + i, "TOKEN", 5u) != 0,
              "the file holds neither the account name nor a token");

    NkAch *reader = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(reader != NULL, "create");
    if (!reader) return;
    char owner[NK_ACH_ACCOUNT_TAG_SIZE];
    CHECK(nk_ach_load_queue_file(reader, path, &report) && report.loaded == 1u &&
              !report.unreadable &&
              !report.save_blocked, "the next session loads it");
    CHECK(nk_ach_copy_pending_owner(reader, owner) && strcmp(owner, player_tag()) == 0,
          "with its owner");
    CHECK(!nk_ach_load_queue_file(reader, path, &report) && report.save_blocked,
          "a second load into a queue that holds records is refused");
    CHECK(!nk_ach_save_queue_file(reader, path), "and the file is then left as it is");
    nk_ach_destroy(reader);

    /* Another account's session: the file's records are not sent, and it is told. */
    NkAch *stranger = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(stranger != NULL, "create");
    if (!stranger) return;
    fake.login_user = "Other";
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(nk_ach_load_queue_file(stranger, path, &report) && report.loaded == 1u, "load");
    CHECK(nk_ach_begin_login(stranger, "Other", "TOKEN2"), "Other signs in");
    nk_ach_replay_pending_unlocks(stranger, 1000u);
    size_t n = take(stranger, ev, NK_ACH_EVENT_QUEUE);
    CHECK(count_requests(&fake, "r=awardachievement&") == 0 &&
              has_event(ev, n, NK_ACH_EVENT_UNLOCKS_DISCARDED) >= 0,
          "a queue file of another account is discarded, reported and not sent");
    CHECK(nk_ach_save_queue_file(stranger, path) &&
              read_bytes(path, bytes, sizeof bytes) == 48u, "the file is rewritten empty");
    nk_ach_destroy(stranger);
    remove_queue_files(path);
}

/* A file in the format of this feature's first, unreleased builds (version 1, no owner) is not a
 * queue this version reads: it is set aside whole, and none of its records is loaded. */
static void test_old_queue_file_is_set_aside(uint8_t *ram) {
    char path[256], aside[NK_MAX_PATH];
    queue_path(path, sizeof path, "ra-queue-old-selftest.bin");
    snprintf(aside, sizeof aside, "%s.unreadable", path);
    remove_queue_files(path);
    Fake fake;
    memset(&fake, 0, sizeof fake);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    const NkAchPendingUnlock saved = saved_unlock(5501u);
    uint8_t bytes[256], read_back[256];
    const size_t size = old_queue_file(bytes, &saved, 1u);
    NkAchQueueLoad report;
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL && write_bytes(path, bytes, size), "a version 1 queue file");
    if (!ach) return;
    CHECK(!nk_ach_load_queue_file(ach, path, &report) && report.unreadable &&
              report.kept_aside && !report.save_blocked && report.loaded == 0u &&
              nk_ach_pending_unlock_count(ach) == 0u,
          "it is not loaded, and is set aside");
    CHECK(!nk_platform_file_exists(path) && read_bytes(aside, read_back, sizeof read_back) == size &&
              memcmp(read_back, bytes, size) == 0, "its bytes are kept under .unreadable");
    nk_ach_destroy(ach);
    remove_queue_files(path);
}

/* A file that cannot be read is kept, never loaded and never overwritten. */
static void test_unreadable_queue_file_is_kept(uint8_t *ram) {
    char path[256], aside[NK_MAX_PATH], aside2[NK_MAX_PATH];
    queue_path(path, sizeof path, "ra-queue-bad-selftest.bin");
    snprintf(aside, sizeof aside, "%s.unreadable", path);
    snprintf(aside2, sizeof aside2, "%s.unreadable.2", path);
    remove_queue_files(path);
    Fake fake;
    memset(&fake, 0, sizeof fake);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAchQueueLoad report;
    uint8_t bytes[256], read_back[256];
    const NkAchPendingUnlock saved = saved_unlock(5501u);

    /* Cut short: a whole record is missing its last byte. */
    size_t size = queue_file(bytes, &saved, 1u) - 1u;
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL && write_bytes(path, bytes, size), "a damaged queue file");
    if (!ach) return;
    CHECK(!nk_ach_load_queue_file(ach, path, &report) && report.unreadable &&
              report.kept_aside && !report.save_blocked && report.loaded == 0u,
          "a damaged file is not loaded, and is set aside");
    CHECK(!nk_platform_file_exists(path) && read_bytes(aside, read_back, sizeof read_back) == size &&
              memcmp(read_back, bytes, size) == 0, "its bytes are kept under .unreadable");
    CHECK(nk_ach_restore_pending_unlock(ach, &saved, player_tag()) &&
              nk_ach_save_queue_file(ach, path), "the run goes on with a new file");
    CHECK(read_bytes(aside, read_back, sizeof read_back) == size &&
              memcmp(read_back, bytes, size) == 0, "which leaves the kept file alone");
    nk_ach_destroy(ach);

    /* A later version's file, with one already set aside: it gets the next name. */
    uint8_t later[64];
    memset(later, 0, sizeof later);
    memcpy(later, "NKRAQ001", 8u);
    put_le(later + 8u, 3u, 4u);
    ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL && write_bytes(path, later, sizeof later), "a later version's file");
    if (!ach) return;
    CHECK(!nk_ach_load_queue_file(ach, path, &report) && report.unreadable &&
              report.kept_aside, "a later version is not read as this one");
    CHECK(read_bytes(aside2, read_back, sizeof read_back) == sizeof later &&
              memcmp(read_back, later, sizeof later) == 0 &&
              read_bytes(aside, read_back, sizeof read_back) == size,
          "it is kept beside the first, which stays");
    nk_ach_destroy(ach);

    /* Every name taken: the file cannot be set aside, so saving is refused and it stays. */
    char name[NK_MAX_PATH];
    for (unsigned n = 3u; n <= 99u; n++) {
        snprintf(name, sizeof name, "%s.unreadable.%u", path, n);
        (void)write_bytes(name, later, 1u);
    }
    ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL && write_bytes(path, bytes, size), "a damaged queue file");
    if (!ach) return;
    CHECK(!nk_ach_load_queue_file(ach, path, &report) && report.unreadable &&
              !report.kept_aside && report.save_blocked, "it could not be set aside");
    CHECK(nk_ach_restore_pending_unlock(ach, &saved, player_tag()), "a record to save");
    CHECK(!nk_ach_save_queue_file(ach, path), "saving is refused for this run");
    CHECK(read_bytes(path, read_back, sizeof read_back) == size &&
              memcmp(read_back, bytes, size) == 0, "the unread file is still there, whole");
    nk_ach_destroy(ach);
    remove_queue_files(path);
}

/* ---- sign-in when the server cannot be reached ------------------------------------------ */

/* A token sign-in the server never answered is due again, with no password asked: after 30
 * seconds, then twice as long each time. An answer from the server, either way, ends it. */
static void test_token_sign_in_is_tried_again_when_the_server_returns(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.login_status = NK_ACH_HTTP_RETRY;
    fake.login_body = "";
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(!nk_ach_token_login_due(ach, 500u), "nothing is due before a sign-in failed");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "the token sign-in starts");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_UNREACHABLE) >= 0 &&
              has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED) < 0 && !nk_ach_logged_in(ach),
          "no connection is not a refused sign-in");
    CHECK(!nk_ach_token_login_due(ach, 1000u), "the wait starts at the first question");
    CHECK(!nk_ach_token_login_due(ach, 30999u), "not due inside 30 s");
    CHECK(nk_ach_token_login_due(ach, 31000u), "due after 30 s");
    CHECK(!nk_ach_token_login_due(ach, 31001u) && !nk_ach_token_login_due(ach, 900000u),
          "due once: the caller signs in again");

    fake.login_status = 503;
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "the second try");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_UNREACHABLE) >= 0, "a failing server is no answer");
    CHECK(!nk_ach_token_login_due(ach, 40000u) && !nk_ach_token_login_due(ach, 99999u),
          "the second wait is twice the first");
    CHECK(nk_ach_token_login_due(ach, 100000u), "due after 60 s");

    fake.login_status = 0;
    fake.login_body = NULL;
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "the server is back");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_OK) >= 0 && nk_ach_logged_in(ach),
          "the stored token signs in, with no password");
    CHECK(count_requests(&fake, "r=login2&") == 3, "three sign-in requests in all");
    for (int i = 0; i < fake.requests && i < 16; i++)
        CHECK(strstr(fake.log[i], "p=") == NULL, "no request carried a password");
    CHECK(!nk_ach_token_login_due(ach, 200000u) && !nk_ach_token_login_due(ach, 2000000u),
          "a sign-in that succeeded is not tried again");
    nk_ach_destroy(ach);

    /* The server refused the token: that is an answer, and asking again cannot change it. */
    memset(&fake, 0, sizeof fake);
    fake.login_body = k_bad_account;
    ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    CHECK(nk_ach_begin_login(ach, "Player", "STALE"), "a stale token");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED) >= 0 &&
              has_event(ev, n, NK_ACH_EVENT_LOGIN_UNREACHABLE) < 0, "the token is refused");
    CHECK(!nk_ach_token_login_due(ach, 1000u) && !nk_ach_token_login_due(ach, 100000u),
          "a refused token is not tried again");

    /* A password is never kept, so a password sign-in has nothing to try again with. */
    fake.login_body = "";
    fake.login_status = NK_ACH_HTTP_RETRY;
    CHECK(nk_ach_begin_login_with_password(ach, "Player", "PASSWORD"), "a password sign-in");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED) >= 0 &&
              has_event(ev, n, NK_ACH_EVENT_LOGIN_UNREACHABLE) < 0 &&
              !nk_ach_token_login_due(ach, 1000u) && !nk_ach_token_login_due(ach, 100000u),
          "an unreachable password sign-in just fails");

    /* Turning the feature off, or signing out, ends a wait in progress. */
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "unreachable again");
    CHECK(!nk_ach_token_login_due(ach, 1000u), "the wait starts");
    nk_ach_logout(ach);
    CHECK(!nk_ach_token_login_due(ach, 100000u), "off: nothing is tried");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "unreachable once more");
    CHECK(!nk_ach_token_login_due(ach, 1000u), "the wait starts");
    (void)nk_ach_sign_out(ach);
    CHECK(!nk_ach_token_login_due(ach, 100000u), "signed out: nothing is tried");
    nk_ach_destroy(ach);
}

/* ---- two threads ------------------------------------------------------------------------ */

/* Every entry into the bridge is inside the lock it was given, the transport is only ever
 * called there, and a done that arrives later, on any thread, takes the lock itself. */
static void test_every_entry_takes_the_lock(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.counts_lock = true;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    const NkAchLock lock = {&fake, lock_enter, lock_leave};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    nk_ach_set_lock(ach, &lock);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    NkAchPendingUnlock record;
    NkAchAchievement list[2];
    NkAchQueueLoad report;
    char text[64], tag[NK_ACH_ACCOUNT_TAG_SIZE], path[NK_MAX_PATH];
    size_t total = 0u;
    queue_path(path, sizeof path, "ra-queue-lock-selftest.bin");
    remove_queue_files(path);
    int before = fake.lock_enters;
#define ENTERS(call, what)                                                                   \
    do {                                                                                     \
        before = fake.lock_enters;                                                           \
        call;                                                                                \
        CHECK(fake.lock_enters > before && fake.lock_depth == 0,                             \
              "%s runs inside the lock and leaves it (%d entries, depth %d)", what,           \
              fake.lock_enters - before, fake.lock_depth);                                   \
    } while (0)
    ENTERS(nk_ach_set_memory(ach, &mem), "nk_ach_set_memory");
    ENTERS((void)nk_ach_save_queue_file(ach, path), "nk_ach_save_queue_file");
    ENTERS((void)nk_ach_load_queue_file(ach, path, &report), "nk_ach_load_queue_file");
    ENTERS((void)nk_ach_begin_login(ach, "Player", "TOKEN"), "nk_ach_begin_login");
    ENTERS((void)nk_ach_token_login_due(ach, 1u), "nk_ach_token_login_due");
    ENTERS((void)nk_ach_copy_user_name(ach, text, sizeof text), "nk_ach_copy_user_name");
    ENTERS((void)nk_ach_copy_user_token(ach, text, sizeof text), "nk_ach_copy_user_token");
    ENTERS((void)nk_ach_logged_in(ach), "nk_ach_logged_in");
    ENTERS((void)nk_ach_begin_load_game(ach, HASH), "nk_ach_begin_load_game");
    ENTERS((void)nk_ach_game_loaded(ach), "nk_ach_game_loaded");
    ENTERS((void)nk_ach_hardcore(ach), "nk_ach_hardcore");
    ENTERS(nk_ach_idle(ach), "nk_ach_idle");
    for (int i = 0; i < 4; i++) ENTERS(nk_ach_frame(ach), "nk_ach_frame");
    ENTERS((void)nk_ach_copy_rich_presence(ach, text, sizeof text), "nk_ach_copy_rich_presence");
    ENTERS((void)nk_ach_copy_achievement_list(ach, list, 2u, &total),
           "nk_ach_copy_achievement_list");

    /* The award request leaves from inside nk_ach_frame, and is answered later, outside. */
    fake.defer = true;
    ram[ACH_ADDR] = 5;
    ENTERS(nk_ach_frame(ach), "nk_ach_frame");
    CHECK(fake.deferred_done != NULL && nk_ach_pending_unlock_count(ach) == 1u,
          "the award is on its way");
    const NkAchResponse answer = {k_award, sizeof k_award - 1u, 200};
    NkAchDone done = fake.deferred_done;
    fake.deferred_done = NULL;
    fake.defer = false;
    if (done) ENTERS(done(&answer, fake.deferred_ctx), "the transport's done");
    CHECK(nk_ach_pending_unlock_count(ach) == 0u, "the answered award left the queue");

    record = saved_unlock(7001u);
    ENTERS((void)nk_ach_restore_pending_unlock(ach, &record, player_tag()),
           "nk_ach_restore_pending_unlock");
    ENTERS((void)nk_ach_pending_unlock_count(ach), "nk_ach_pending_unlock_count");
    ENTERS((void)nk_ach_copy_pending_unlocks(ach, &record, 1u), "nk_ach_copy_pending_unlocks");
    ENTERS((void)nk_ach_copy_pending_owner(ach, tag), "nk_ach_copy_pending_owner");
    ENTERS((void)nk_ach_save_queue_file(ach, path), "nk_ach_save_queue_file");
    fake.defer = true;
    ENTERS(nk_ach_replay_pending_unlocks(ach, 1000u), "nk_ach_replay_pending_unlocks");
    done = fake.deferred_done;
    fake.deferred_done = NULL;
    fake.defer = false;
    CHECK(done != NULL, "the saved unlock is on its way");
    if (done) ENTERS(done(&answer, fake.deferred_ctx), "the replay's done");
    ENTERS((void)take(ach, ev, NK_ACH_EVENT_QUEUE), "nk_ach_take_events");
    ENTERS((void)nk_ach_events_dropped(ach), "nk_ach_events_dropped");
    ENTERS((void)nk_ach_begin_login_with_password(ach, "Player", "PASSWORD"),
           "nk_ach_begin_login_with_password");
    ENTERS(nk_ach_logout(ach), "nk_ach_logout");
    ENTERS((void)nk_ach_sign_out(ach), "nk_ach_sign_out");
    ENTERS(nk_ach_destroy(ach), "nk_ach_destroy");
#undef ENTERS
    CHECK(fake.requests > 4 && fake.transport_calls_outside_lock == 0,
          "the transport was always called inside the lock (%d of %d calls were not)",
          fake.transport_calls_outside_lock, fake.requests);
    remove_queue_files(path);
}

/* The last achievement of a set, answered just as the game ends. rcheevos 12.5.0 would read
 * through a null pointer when the game is then unloaded, or when the answer comes after it. */
static void test_last_achievement_answered_as_the_game_ends(uint8_t *ram) {
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    const NkAchResponse answer = {k_award, sizeof k_award - 1u, 200};
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(strstr(k_award, "\"AchievementsRemaining\":0") != NULL, "the answer completes the set");
    /* 0: the answer, then the game is unloaded. 1: unloaded, then the answer. 2: the answer,
     * then the player closes. */
    for (int order = 0; order < 3; order++) {
        Fake fake;
        memset(&fake, 0, sizeof fake);
        fake.award_status = 200;
        memset(ram, 0, USER_RAM_SIZE);
        NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
        CHECK(ach != NULL, "create");
        if (!ach) return;
        nk_ach_begin_login(ach, "Player", "TOKEN");
        nk_ach_begin_load_game(ach, HASH);
        for (int i = 0; i < 4; i++) nk_ach_frame(ach);
        fake.defer = true;
        ram[ACH_ADDR] = 5;
        nk_ach_frame(ach);
        NkAchDone done = fake.deferred_done;
        CHECK(done != NULL && nk_ach_pending_unlock_count(ach) == 1u, "the award is on its way");
        if (order == 1) nk_ach_logout(ach);
        if (done) done(&answer, fake.deferred_ctx);
        CHECK(nk_ach_pending_unlock_count(ach) == 0u,
              "order %d: the answered award left the queue", order);
        if (order == 0) nk_ach_logout(ach);
        (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
        nk_ach_destroy(ach);
    }
}

/* ---- rc_client's own retries ------------------------------------------------------------ */

#define HASH2 "fedcba9876543210fedcba9876543210"

/* rc_client schedules a retry by its clock; the tests turn it by hand. */
static rc_clock_t g_clock_ms;

static rc_clock_t RC_CCONV fake_clock(const rc_client_t *client) {
    (void)client;
    return g_clock_ms;
}

static void use_fake_clock(NkAch *ach) {
    rc_client_set_get_time_millisecs_function(nk_ach_rc_client(ach), fake_clock);
}

static void idle_for(NkAch *ach, rc_clock_t ms) {
    const rc_clock_t end = g_clock_ms + ms;
    while (g_clock_ms < end) {
        g_clock_ms += 500u;
        nk_ach_idle(ach);
    }
}

/* Signs in, loads the synthetic game and earns 5501; the fake answers the award, or keeps its
 * done when defer_award. */
static void earn_five(NkAch *ach, Fake *fake, uint8_t *ram, const char *user, const char *token,
                      const char *hash, bool defer_award) {
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    memset(ram, 0, USER_RAM_SIZE);
    CHECK(nk_ach_begin_login(ach, user, token) && nk_ach_begin_load_game(ach, hash),
          "%s signs in and loads the game", user);
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    fake->defer = defer_award;
    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    fake->defer = false;
}

static int awards_sent_as(const Fake *f, const char *user_field) {
    int count = 0;
    for (int i = 0; i < f->requests && i < 16; i++)
        if (starts_with(f->log[i], "r=awardachievement&") && strstr(f->log[i], user_field))
            count++;
    return count;
}

/* An award rc_client could not send waits on its scheduler and is rebuilt, when it fires, from
 * whoever is signed in then. Once its account has ended (Sign out, or the switch turned off and
 * another account signing in), it must never go out under the next account. */
static void test_award_retries_end_with_their_account(uint8_t *ram) {
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    for (int route = 0; route < 2; route++) {
        Fake fake;
        memset(&fake, 0, sizeof fake);
        fake.award_status = NK_ACH_HTTP_RETRY;
        NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
        CHECK(ach != NULL, "create");
        if (!ach) return;
        g_clock_ms = 1000000u;
        use_fake_clock(ach);
        earn_five(ach, &fake, ram, "Player", "TOKEN", HASH, false);
        CHECK(awards_sent_as(&fake, "u=Player&") == 2,
              "route %d: the award and rc_client's first retry found no server (%d)", route,
              awards_sent_as(&fake, "u=Player&"));
        if (route == 0) (void)nk_ach_sign_out(ach);
        else nk_ach_logout(ach);
        fake.award_status = 200;
        fake.login_user = "Other";
        use_fake_clock(ach);
        CHECK(nk_ach_begin_login(ach, "Other", "TOKEN2") && nk_ach_logged_in(ach),
              "route %d: another account signs in", route);
        idle_for(ach, 130000u);
        CHECK(awards_sent_as(&fake, "u=Other&") == 0,
              "route %d: Player's unlock is never sent as Other's (%d award requests as Other)",
              route, awards_sent_as(&fake, "u=Other&"));
        nk_ach_destroy(ach);
    }
}

/* A retry outlives its game: when it is answered after another game started, its own record
 * leaves the queue. */
static void test_award_retry_keeps_its_game(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = NK_ACH_HTTP_RETRY;
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    g_clock_ms = 1000000u;
    use_fake_clock(ach);
    earn_five(ach, &fake, ram, "Player", "TOKEN", HASH, false);
    NkAchPendingUnlock queued;
    CHECK(nk_ach_copy_pending_unlocks(ach, &queued, 1u) == 1u &&
              strcmp(queued.game_hash, HASH) == 0, "the unsent unlock is queued for its game");
    fake.award_status = 200;
    CHECK(nk_ach_begin_load_game(ach, HASH2), "another game starts");
    idle_for(ach, 5000u);
    const int award = fake.requests - 1;
    CHECK(count_requests(&fake, "r=awardachievement&") == 3 && award >= 0 && award < 16 &&
              starts_with(fake.log[award], "r=awardachievement&") &&
              strstr(fake.log[award], "m=" HASH) != NULL,
          "rc_client's retry is sent for the first game (%d award requests)",
          count_requests(&fake, "r=awardachievement&"));
    CHECK(nk_ach_pending_unlock_count(ach) == 0u,
          "the answered retry's record left the queue (%u left)",
          (unsigned)nk_ach_pending_unlock_count(ach));
    nk_ach_destroy(ach);
}

/* The server refuses a live award by its own error: rc_client gives it up, so its record leaves
 * the queue now, reported, instead of being saved and refused again next session. */
static void test_refused_live_award_leaves_the_queue(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.refuse_award = "a=5501&";
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    earn_five(ach, &fake, ram, "Player", "TOKEN", HASH, true);
    CHECK(fake.deferred_done != NULL && nk_ach_pending_unlock_count(ach) == 1u,
          "the award is on its way, its record queued");
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    const NkAchResponse refused = {k_refused, sizeof k_refused - 1u, 200};
    if (fake.deferred_done) fake.deferred_done(&refused, fake.deferred_ctx);
    CHECK(nk_ach_pending_unlock_count(ach) == 0u, "the refused unlock left the queue (%u left)",
          (unsigned)nk_ach_pending_unlock_count(ach));
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int told = has_event(ev, n, NK_ACH_EVENT_UNLOCK_REFUSED);
    CHECK(told >= 0 && ev[told].id == 5501u && has_event(ev, n, NK_ACH_EVENT_QUEUE_CHANGED) >= 0,
          "the refusal is reported and the queue file rewritten");
    nk_ach_destroy(ach);

    /* Answered before rc_client raised the unlock: no record is made for it. */
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.refuse_award = "a=5501&";
    ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    earn_five(ach, &fake, ram, "Player", "TOKEN", HASH, false);
    CHECK(count_requests(&fake, "r=awardachievement&") == 1 &&
              nk_ach_pending_unlock_count(ach) == 0u,
          "a refusal answered at once leaves no record (%u queued)",
          (unsigned)nk_ach_pending_unlock_count(ach));
    nk_ach_destroy(ach);
}

/* An award answered after its account signed out does not change what the next account's
 * unlocks do: one it cannot send is still queued. */
static void test_late_answer_after_sign_out_keeps_the_next_unlock(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    earn_five(ach, &fake, ram, "Player", "TOKEN", HASH, true);
    NkAchDone done = fake.deferred_done;
    void *done_ctx = fake.deferred_ctx;
    fake.deferred_done = NULL;
    CHECK(done != NULL, "Player's award is on its way");
    CHECK(nk_ach_sign_out(ach) == 1u, "Player signs out");
    const NkAchResponse answer = {k_award, sizeof k_award - 1u, 200};
    if (done) done(&answer, done_ctx);
    CHECK(nk_ach_pending_unlock_count(ach) == 0u, "the late answer adds nothing");

    fake.login_user = "Other";
    fake.award_status = NK_ACH_HTTP_RETRY;
    earn_five(ach, &fake, ram, "Other", "TOKEN2", HASH, false);
    CHECK(nk_ach_pending_unlock_count(ach) == 1u,
          "Other's unlock, which found no server, is queued (%u queued)",
          (unsigned)nk_ach_pending_unlock_count(ach));
    nk_ach_destroy(ach);
}

/* The player's transport carries one request at a time. A saved unlock on its way when a game
 * starts must not cost that game its achievement set: rcheevos does not retry a set fetch, so
 * the bridge holds each request until the one before it is answered, in the order they came. */
static void test_requests_wait_for_the_one_transport_slot(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    const NkAchPendingUnlock saved = saved_unlock(5501u);
    CHECK(nk_ach_restore_pending_unlock(ach, &saved, player_tag()), "restore");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "sign in");
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);

    fake.defer = true;
    fake.one_slot = true;
    nk_ach_replay_pending_unlocks(ach, 1000u);
    CHECK(fake.deferred_done != NULL && count_requests(&fake, "r=awardachievement&") == 1,
          "the saved unlock holds the slot");
    CHECK(nk_ach_begin_load_game(ach, HASH), "a game starts while it is on its way");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_GAME_FAILED) < 0 && fake.slot_refusals == 0,
          "the set fetch waits for the slot instead of failing (%d refused by the busy "
          "transport)", fake.slot_refusals);
    CHECK(count_requests(&fake, "r=achievementsets&") == 0, "nothing else is sent meanwhile");

    for (int answers = 0; answers < 8 && fake_answer_held(&fake); answers++) {}
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(fake.slot_refusals == 0 && nk_ach_pending_unlock_count(ach) == 0u,
          "the unlock was answered and left the queue (%d refused, %u queued)",
          fake.slot_refusals, (unsigned)nk_ach_pending_unlock_count(ach));
    const int award = find_request(&fake, "r=awardachievement&");
    const int sets = find_request(&fake, "r=achievementsets&");
    CHECK(award >= 0 && sets > award, "the set fetch left after the unlock (%d, %d)", award,
          sets);
    CHECK(has_event(ev, n, NK_ACH_EVENT_GAME_LOADED) >= 0 &&
              has_event(ev, n, NK_ACH_EVENT_GAME_FAILED) < 0,
          "the game's set is loaded");
    fake.defer = false;
    nk_ach_destroy(ach);
}

static int requests_naming_from(const Fake *f, int first, const char *field) {
    int count = 0;
    for (int i = first; i < f->requests && i < 16; i++)
        if (strstr(f->log[i], field)) count++;
    return count;
}

/* Requests still waiting for the one transport slot when the account signs out carry its name
 * and token: none of them is sent once the request holding the slot is answered. Route 0: a set
 * fetch holds the slot and a saved unlock waits; route 1: the other way round. */
static void test_requests_waiting_at_sign_out_are_not_sent(uint8_t *ram) {
    for (int route = 0; route < 2; route++) {
        Fake fake;
        memset(&fake, 0, sizeof fake);
        fake.award_status = 200;
        memset(ram, 0, USER_RAM_SIZE);
        const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
        NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
        CHECK(ach != NULL, "create");
        if (!ach) return;
        NkAchEvent ev[NK_ACH_EVENT_QUEUE];
        const NkAchPendingUnlock saved = saved_unlock(5501u);
        CHECK(nk_ach_restore_pending_unlock(ach, &saved, player_tag()), "restore");
        CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "sign in");
        (void)take(ach, ev, NK_ACH_EVENT_QUEUE);

        fake.defer = true;
        fake.one_slot = true;
        if (route == 0) {
            CHECK(nk_ach_begin_load_game(ach, HASH), "route 0: a game starts");
            nk_ach_replay_pending_unlocks(ach, 1000u);
            CHECK(count_requests(&fake, "r=awardachievement&") == 0,
                  "route 0: the saved unlock waits for the slot");
        } else {
            nk_ach_replay_pending_unlocks(ach, 1000u);
            CHECK(nk_ach_begin_load_game(ach, HASH), "route 1: a game starts");
            CHECK(count_requests(&fake, "r=awardachievement&") == 1 &&
                      count_requests(&fake, "r=achievementsets&") == 0,
                  "route 1: the set fetch waits for the slot");
        }
        const int sent = fake.requests;
        CHECK(fake.deferred_done != NULL && sent >= 1, "route %d: one request holds the slot",
              route);
        CHECK(nk_ach_sign_out(ach) == 1u, "route %d: Player signs out", route);
        for (int answers = 0; answers < 8 && fake_answer_held(&fake); answers++) {}
        CHECK(fake.requests == sent && requests_naming_from(&fake, sent, "u=Player&") == 0 &&
                  requests_naming_from(&fake, sent, "t=TOKEN") == 0,
              "route %d: nothing of Player's is sent after the sign-out (%d sent, %d of "
              "Player's)", route, fake.requests - sent,
              requests_naming_from(&fake, sent, "u=Player&"));
        CHECK(fake.slot_refusals == 0, "route %d: the busy transport refused nothing", route);
        fake.defer = false;
        fake.one_slot = false;
        nk_ach_destroy(ach);
    }
}

/* A saved unlock on its way when its account signs out is answered after the next account has
 * signed in and saved an unlock of its own for the same game and achievement. That answer is
 * the old account's: it neither removes the next account's record (route 0, awarded) nor holds
 * its queue back as a refused account (route 1). */
static void test_late_replay_answer_leaves_the_next_account(uint8_t *ram) {
    char other[NK_ACH_ACCOUNT_TAG_SIZE];
    CHECK(nk_ach_account_tag("Other", other), "Other's tag");
    for (int route = 0; route < 2; route++) {
        Fake fake;
        memset(&fake, 0, sizeof fake);
        fake.award_status = 200;
        fake.login_user = "Other";
        memset(ram, 0, USER_RAM_SIZE);
        const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
        NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
        CHECK(ach != NULL, "create");
        if (!ach) return;
        NkAchEvent ev[NK_ACH_EVENT_QUEUE];
        const NkAchPendingUnlock saved = saved_unlock(5501u);
        CHECK(nk_ach_restore_pending_unlock(ach, &saved, player_tag()), "restore Player's");
        fake.login_user = NULL;
        CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "Player signs in");
        fake.login_user = "Other";
        fake.defer = true;
        fake.one_slot = true;
        nk_ach_replay_pending_unlocks(ach, 1000u);
        NkAchDone done = fake.deferred_done;
        void *done_ctx = fake.deferred_ctx;
        fake.deferred_done = NULL;
        CHECK(done != NULL && count_requests(&fake, "r=awardachievement&") == 1,
              "route %d: Player's saved unlock is on its way", route);
        CHECK(nk_ach_sign_out(ach) == 1u, "route %d: Player signs out", route);

        fake.defer = false;
        fake.one_slot = false;
        CHECK(nk_ach_restore_pending_unlock(ach, &saved, other), "restore Other's");
        CHECK(nk_ach_begin_login(ach, "Other", "TOKEN2"), "route %d: Other signs in", route);
        (void)take(ach, ev, NK_ACH_EVENT_QUEUE);

        const NkAchResponse awarded = {k_award, sizeof k_award - 1u, 200};
        const NkAchResponse bad_account = {k_bad_account, sizeof k_bad_account - 1u, 401};
        if (done) done(route == 0 ? &awarded : &bad_account, done_ctx);
        size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
        CHECK(nk_ach_pending_unlock_count(ach) == 1u,
              "route %d: Other's record is still queued (%u queued)", route,
              (unsigned)nk_ach_pending_unlock_count(ach));
        CHECK(has_event(ev, n, NK_ACH_EVENT_SERVER_ERROR) < 0 &&
                  has_event(ev, n, NK_ACH_EVENT_UNLOCK_REFUSED) < 0 &&
                  has_event(ev, n, NK_ACH_EVENT_QUEUE_CHANGED) < 0,
              "route %d: the late answer tells Other nothing", route);

        const int sent = fake.requests;
        nk_ach_replay_pending_unlocks(ach, 2000u);
        CHECK(requests_naming_from(&fake, sent, "r=awardachievement&") == 1 &&
                  requests_naming_from(&fake, sent, "u=Other&") == 1,
              "route %d: Other's saved unlock is sent with Other's account (%d sent)", route,
              fake.requests - sent);
        CHECK(nk_ach_pending_unlock_count(ach) == 0u, "route %d: and answered", route);
        nk_ach_destroy(ach);
    }
}

/* Turning the feature off while a token sign-in is on its way: rc_client answers it as
 * aborted once the server's reply arrives. A reply that found no server must not arm the token
 * sign-in's retry for a feature that is now off. */
static void test_sign_in_ended_by_logout_is_not_tried_again(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.defer = true;
    fake.login_status = NK_ACH_HTTP_RETRY;
    fake.login_body = "";
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "the token sign-in starts");
    CHECK(fake.deferred_done != NULL, "the sign-in is on its way");
    nk_ach_logout(ach);
    CHECK(fake_answer_held(&fake), "the reply, which found no server, arrives after the logout");
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int failed = has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED);
    CHECK(failed >= 0 && has_event(ev, n, NK_ACH_EVENT_LOGIN_UNREACHABLE) < 0 &&
              !nk_ach_logged_in(ach),
          "an aborted sign-in ends as failed, not unreachable (failed at %d)", failed);
    CHECK(!nk_ach_token_login_due(ach, 1000u) && !nk_ach_token_login_due(ach, 100000u) &&
              !nk_ach_token_login_due(ach, 2000000u),
          "an aborted sign-in is not tried again");
    fake.defer = false;
    nk_ach_destroy(ach);
}

/* True when text[0..length) ends on a UTF-8 character boundary: its last lead byte starts a
 * sequence whose continuation bytes are all there. */
static bool utf8_ends_whole(const char *text) {
    const size_t length = strlen(text);
    size_t lead = length;
    while (lead > 0u && ((unsigned char)text[lead - 1u] & 0xC0u) == 0x80u) lead--;
    if (lead == 0u) return length == 0u;
    const unsigned char first = (unsigned char)text[lead - 1u];
    const size_t want = first < 0x80u ? 1u : first >= 0xF0u ? 4u : first >= 0xE0u ? 3u : 2u;
    return length - (lead - 1u) == want;
}

/* RetroAchievements titles are UTF-8. One longer than the bridge's event or list field is cut
 * at a character, never inside one, so the toast and the launcher get valid text. */
static void test_long_titles_are_cut_at_a_character(uint8_t *ram) {
    static char sets[4096];
    char title[64 * 3 + 1] = "";
    char description[80 * 3 + 1] = "";
    for (int i = 0; i < 64; i++) memcpy(title + i * 3, "\xE3\x81\x82", 4u);
    for (int i = 0; i < 80; i++) memcpy(description + i * 3, "\xE3\x81\x84", 4u);
    snprintf(sets, sizeof sets,
             "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
             "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
             "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,"
             "\"Type\":\"core\",\"ImageIconUrl\":\"\",\"Achievements\":["
             "{\"ID\":5501,\"Title\":\"%s\",\"Description\":\"%s\","
             "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800100=5\",\"Author\":\"Synthetic\","
             "\"BadgeName\":\"00001\",\"Created\":1700000000,\"Modified\":1700000000}"
             "],\"Leaderboards\":[]}]}", title, description);
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.sets_body = sets;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "sign in");
    CHECK(nk_ach_begin_load_game(ach, HASH), "load");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_GAME_LOADED) >= 0, "the set with long titles loads");
    NkAchAchievement list[2];
    size_t total = 0u;
    const size_t count = nk_ach_copy_achievement_list(ach, list, 2u, &total);
    CHECK(count == 1u, "one achievement listed (%u)", (unsigned)count);
    if (count == 1u) {
        CHECK(strlen(list[0].title) > 0u && utf8_ends_whole(list[0].title),
              "the list title ends on a character (%u bytes)", (unsigned)strlen(list[0].title));
        CHECK(strlen(list[0].description) > 0u && utf8_ends_whole(list[0].description),
              "the description ends on a character (%u bytes)",
              (unsigned)strlen(list[0].description));
    }
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int unlocked = has_event(ev, n, NK_ACH_EVENT_UNLOCKED);
    CHECK(unlocked >= 0, "the achievement unlocks");
    if (unlocked >= 0)
        CHECK(strlen(ev[unlocked].title) > 0u && utf8_ends_whole(ev[unlocked].title),
              "the unlock event's title ends on a character (%u bytes)",
              (unsigned)strlen(ev[unlocked].title));
    nk_ach_destroy(ach);
}

/* ---- frames while the player's own loop stands still -------------------------------------- */

static const char k_sets_hits[] =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
    "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
    "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"\",\"Achievements\":["
    "{\"ID\":5601,\"Title\":\"Sixty\",\"Description\":\"The watched byte reads 1 on 60 frames\","
    "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800100=1.60.\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00003\",\"Created\":1700000000,\"Modified\":1700000000}"
    "],\"Leaderboards\":[]}]}";

#define CHANNEL_ARENA 0x10000u
#define CHILD_DEADLINE_MS 60000

/* The runtime's side, in a process of its own as in a real launch: for each "<value>x<count>"
 * of the script it writes the watched byte, then publishes that many VBLANKs, waiting on each
 * for the player's acknowledgement. Exit 0 when every VBLANK was acknowledged. */
static int runtime_child(const char *channel_name, const char *script) {
    NkGuestMemoryChannel channel;
    if (!nk_platform_guest_memory_channel_open(&channel, channel_name, CHANNEL_ARENA)) return 4;
    const char *p = script;
    while (*p) {
        char *end = NULL;
        const unsigned long value = strtoul(p, &end, 10);
        if (end == p || *end != 'x') return 5;
        p = end + 1;
        const unsigned long frames = strtoul(p, &end, 10);
        if (end == p) return 5;
        p = *end == ',' ? end + 1 : end;
        for (unsigned long i = 0u; i < frames; i++) {
            channel.arena[ACH_ADDR] = (uint8_t)value;
            if (!nk_platform_guest_memory_channel_sync_vblank(&channel)) return 3;
        }
    }
    nk_platform_guest_memory_channel_close(&channel);
    return 0;
}

typedef struct FrameService {
    NkAch *ach;
    volatile int frames;
} FrameService;

/* What the player's frame service does for each VBLANK (src/player/main.c,
 * player_ach_serve_frame): one evaluation, inside the bridge's lock. */
static void serve_frame(void *context) {
    FrameService *service = (FrameService *)context;
    nk_ach_frame(service->ach);
    service->frames++;
}

/* Runs the runtime's script to its end while this thread, the player's own, does nothing but
 * wait for it: no event is pumped and the bridge is not touched from here. The exit code, or
 * -1 when the runtime was still waiting for an acknowledgement at the deadline. */
static int run_runtime_while_stalled(const char *self, const char *channel_name,
                                     const char *script) {
    const char *argv[5] = {self, "--runtime-child", channel_name, script, NULL};
    NkProcessHandle child;
    memset(&child, 0, sizeof child);
    if (!nk_platform_spawn_process(self, argv, NULL, NULL, NULL, &child)) return -2;
    const int code = nk_platform_wait_process(&child, CHILD_DEADLINE_MS);
    if (nk_platform_is_process_running(&child)) nk_platform_terminate_process(&child);
    nk_platform_close_process(&child);
    return code;
}

/* A modal box or a dragged window stops the player's event loop for as long as it lasts. The
 * game must go on, and the achievements must still see every frame exactly once: the runtime's
 * VBLANKs are served by the channel's own thread. An achievement that needs its byte to read 1
 * on 60 frames counts them: 59 are not enough, and no frame may count twice. */
static void test_frames_are_served_while_the_event_loop_is_stalled(void) {
    char self[NK_MAX_PATH], channel_name[NK_GUEST_MEMORY_CHANNEL_NAME_MAX];
    NkGuestMemoryChannel channel;
    snprintf(channel_name, sizeof channel_name, "Local\\NakagawaRecomp.Selftest.GuestMemory.%lu",
             nk_platform_process_id());
    const bool created = nk_platform_guest_memory_channel_create(&channel, channel_name,
                                                                 CHANNEL_ARENA);
#ifdef _WIN32
    CHECK(created, "the guest-memory channel is created");
#endif
    if (!created) {
        printf("test_nk_achievements: SKIP the frame service: this host has no guest-memory "
               "channel\n");
        return;
    }
    CHECK(nk_platform_get_executable_path(self, sizeof self), "this test's own path");
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.sets_body = k_sets_hits;
    fake.counts_lock = true;
    const NkAchMemory mem = {channel.arena, USER_RAM, CHANNEL_ARENA};
    const NkAchLock lock = {&fake, lock_enter, lock_leave};
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    FrameService service = {nk_ach_create(&mem, fake_transport, &fake), 0};
    CHECK(service.ach != NULL, "create");
    if (!service.ach) {
        nk_platform_guest_memory_channel_close(&channel);
        return;
    }
    nk_ach_set_lock(service.ach, &lock);
    CHECK(nk_ach_begin_login(service.ach, "Player", "TOKEN") &&
              nk_ach_begin_load_game(service.ach, HASH), "sign in and load the game");
    size_t n = take(service.ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_GAME_LOADED) >= 0, "the game is loaded");
    CHECK(nk_platform_guest_memory_channel_serving(&channel) == 0, "not served yet");
    CHECK(!nk_platform_guest_memory_channel_runtime_attached(&channel),
          "no runtime has opened the channel yet");
    CHECK(nk_platform_guest_memory_channel_serve(&channel, serve_frame, &service),
          "the frame service starts");
    CHECK(!nk_platform_guest_memory_channel_serve(&channel, serve_frame, &service),
          "one service per channel");
    CHECK(nk_platform_guest_memory_channel_serving(&channel) == 1, "frames are served");

    /* 4 frames reading 0, 59 reading 1, 5 reading 0: one hit short. */
    int code = run_runtime_while_stalled(self, channel_name, "0x4,1x59,0x5");
    CHECK(code == 0, "the runtime got all 68 frames acknowledged while the player's loop stood "
                     "still (exit %d; -1: it was still waiting after %d s)", code,
          CHILD_DEADLINE_MS / 1000);
    CHECK(nk_platform_guest_memory_channel_runtime_attached(&channel),
          "the runtime that opened the channel says so: a runtime that ignores the channel "
          "can be told apart");
    CHECK(service.frames == 68, "each VBLANK was evaluated once (%d evaluations of 68)",
          service.frames);
    n = take(service.ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKED) < 0 &&
              count_requests(&fake, "r=awardachievement&") == 0,
          "59 frames are not 60: nothing unlocks");

    /* The 60th. */
    code = run_runtime_while_stalled(self, channel_name, "1x1");
    CHECK(code == 0 && service.frames == 69, "one more frame (exit %d, %d evaluations)", code,
          service.frames);
    n = take(service.ach, ev, NK_ACH_EVENT_QUEUE);
    const int unlocked = has_event(ev, n, NK_ACH_EVENT_UNLOCKED);
    CHECK(unlocked >= 0 && ev[unlocked].id == 5601u, "the 60th frame unlocks it");
    CHECK(count_requests(&fake, "r=awardachievement&") == 1 &&
              fake.transport_calls_outside_lock == 0,
          "its award left once, from inside the lock (%d requests, %d outside)",
          count_requests(&fake, "r=awardachievement&"), fake.transport_calls_outside_lock);

    nk_platform_guest_memory_channel_stop(&channel);
    nk_platform_guest_memory_channel_close(&channel);
    CHECK(channel.view == NULL && fake.lock_depth == 0,
          "closing waits for the service, which left the lock");
    nk_ach_destroy(service.ach);
}

/* ---- maintainer requirements (#1316, 2026-11-20) and audit F-282bdd0c, F-6c138dc7 ----------- */

/* A set with a delta trigger (5601: the byte was 4 and is now 5) and a hit-count trigger (5602:
 * the byte at 0x110 reads 1 on three frames). */
static const char k_sets_state[] =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
    "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
    "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"\",\"Achievements\":["
    "{\"ID\":5601,\"Title\":\"Delta\",\"Description\":\"The byte goes from 4 to 5\","
    "\"Flags\":3,\"Points\":5,\"MemAddr\":\"d0xH800100=4_0xH800100=5\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00001\",\"Created\":1700000000,\"Modified\":1700000000},"
    "{\"ID\":5602,\"Title\":\"Hits\",\"Description\":\"The byte reads 1 three times\","
    "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800110=1.3.\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00002\",\"Created\":1700000000,\"Modified\":1700000000}"
    "],\"Leaderboards\":[]}]}";

static NkAch *signed_in_with_set(uint8_t *ram, Fake *fake, const char *sets) {
    memset(fake, 0, sizeof *fake);
    fake->award_status = 200;
    fake->sets_body = sets;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, fake);
    CHECK(ach != NULL, "create");
    if (!ach) return NULL;
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN") && nk_ach_logged_in(ach), "signed in");
    CHECK(nk_ach_begin_load_game(ach, HASH) && nk_ach_game_loaded(ach), "set loaded");
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    return ach;
}

static bool unlocked_now(NkAch *ach, uint32_t id) {
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    for (size_t i = 0; i < n; i++)
        if (ev[i].kind == NK_ACH_EVENT_UNLOCKED && ev[i].id == id) return true;
    return false;
}

static int requests_holding(const Fake *f, const char *text) {
    int count = 0;
    for (int i = 0; i < f->requests && i < 16; i++)
        if (strstr(f->log[i], text)) count++;
    return count;
}

/* Softcore only: hardcore turned on in rc_client by any route is off again before the next frame
 * is evaluated, the unlock is sent as softcore, and a request that would record hardcore play
 * never reaches the transport. */
static void test_hardcore_cannot_be_turned_on(uint8_t *ram) {
    Fake fake;
    NkAch *ach = signed_in_with_set(ram, &fake, NULL);
    if (!ach) return;
    rc_client_t *client = nk_ach_rc_client(ach);
    for (int i = 0; i < 3; i++) nk_ach_frame(ach);
    rc_client_set_hardcore_enabled(client, 1);
    nk_ach_frame(ach);
    CHECK(!nk_ach_hardcore(ach) && !rc_client_get_hardcore_enabled(client),
          "hardcore turned on in rc_client is off again after the next frame");
    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    const int award = find_request(&fake, "r=awardachievement&");
    CHECK(award >= 0 && strstr(fake.log[award], "&h=0") != NULL,
          "the unlock is sent as softcore: %s", award >= 0 ? fake.log[award] : "(none)");

    /* rc_client driven directly, past every bridge entry: its start of session says h=1, and
     * the request gate refuses it. */
    rc_client_set_hardcore_enabled(client, 1);
    const int before = fake.requests;
    (void)rc_client_begin_load_game(client, HASH2, NULL, NULL);
    CHECK(requests_holding(&fake, "&h=1") == 0,
          "no request that records hardcore play reaches the transport (%d sent since)",
          fake.requests - before);
    nk_ach_frame(ach);
    CHECK(!nk_ach_hardcore(ach), "and hardcore is off again");
    nk_ach_destroy(ach);
}

/* The switch off: nothing reaches the transport any more. A request waiting its turn is answered
 * here, rc_client's own retries and a saved unlock stay unsent, and the record stays for later. */
static void test_switched_off_sends_nothing(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    const NkAchPendingUnlock saved = saved_unlock(5501u);
    CHECK(nk_ach_restore_pending_unlock(ach, &saved, player_tag()), "restore");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "sign in");
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(nk_ach_online(ach), "on by default once a transport is given");

    fake.defer = true;
    fake.one_slot = true;
    CHECK(nk_ach_begin_load_game(ach, HASH), "a game starts: its set fetch holds the slot");
    nk_ach_replay_pending_unlocks(ach, 1000u);
    const int sent = fake.requests;
    CHECK(sent >= 1 && fake.deferred_done != NULL, "one request is held (%d sent)", sent);

    nk_ach_set_online(ach, false);
    CHECK(!nk_ach_online(ach), "off");
    (void)fake_answer_held(&fake);
    fake.defer = false;
    fake.one_slot = false;
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    for (int i = 0; i < 4; i++) nk_ach_idle(ach);
    nk_ach_replay_pending_unlocks(ach, 1000000u);
    (void)nk_ach_begin_login(ach, "Player", "TOKEN");
    (void)nk_ach_begin_load_game(ach, HASH);
    CHECK(fake.requests == sent,
          "nothing reaches the transport once the switch is off (%d more)", fake.requests - sent);
    CHECK(nk_ach_pending_unlock_count(ach) == 1u, "the saved unlock stays for later");

    nk_ach_set_online(ach, true);
    (void)nk_ach_begin_login(ach, "Player", "TOKEN");
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    nk_ach_replay_pending_unlocks(ach, 2000000u);
    CHECK(requests_holding(&fake, "r=awardachievement&") >= 1,
          "on again, the saved unlock is sent");
    nk_ach_destroy(ach);
}

/* The user agent RetroAchievements documents: <product>/<numeric version> (<system>) and
 * rc_client's clause, naming this player and nothing else. */
static void test_user_agent_names_this_player(void) {
    char clause[64];
    (void)rc_client_get_user_agent_clause(NULL, clause, sizeof clause);
    char expected[NK_ACH_USER_AGENT_MAX];
    snprintf(expected, sizeof expected, "NakagawaRecomp/0.1.0 (Windows 10.0.26100) %s", clause);
    char ua[NK_ACH_USER_AGENT_MAX];
    CHECK(strncmp(clause, "rcheevos/", 9u) == 0, "rc_client's clause names rcheevos: %s", clause);
    /* The version in the user agent is the vendored one, not a copy that could drift from it. */
    CHECK(strcmp(clause, "rcheevos/" RCHEEVOS_VERSION_STRING) == 0,
          "the clause \"%s\" names the vendored rcheevos " RCHEEVOS_VERSION_STRING, clause);
    CHECK(nk_ach_user_agent("0.1.0-dev", "Windows 10.0.26100", ua, sizeof ua) &&
              strcmp(ua, expected) == 0,
          "the user agent is \"%s\", not \"%s\"", ua, expected);
    snprintf(expected, sizeof expected, "NakagawaRecomp/1.2 %s", clause);
    CHECK(nk_ach_user_agent("1.2", NULL, ua, sizeof ua) && strcmp(ua, expected) == 0,
          "no system part when none is known: \"%s\"", ua);
    static const char *const refused_versions[] = {"", "dev", "v0.1.0", "0..1", ".1", "1.",
                                                   "1.2.3.4.5", "0.1.0dev", "1234567890"};
    for (size_t i = 0; i < sizeof refused_versions / sizeof refused_versions[0]; i++) {
        CHECK(!nk_ach_user_agent(refused_versions[i], "Windows", ua, sizeof ua) && ua[0] == '\0',
              "version \"%s\" has no numeric form and is refused", refused_versions[i]);
    }
    CHECK(!nk_ach_user_agent(NULL, "Windows", ua, sizeof ua), "no version is refused");
    CHECK(!nk_ach_user_agent("0.1.0", "Windows (x64)", ua, sizeof ua),
          "a parenthesis would end the system part early");
    CHECK(!nk_ach_user_agent("0.1.0", "Windows\n", ua, sizeof ua), "a control character");
    CHECK(!nk_ach_user_agent("0.1.0", "Windows", ua, 20u) && ua[0] == '\0', "too small a buffer");
}

/* Overwrites nothing: reads what the frames below the caller left on the stack. Reading it
 * uninitialized is the point; unsigned char has no trap representation, and volatile keeps every
 * read, so the compiler's warning about it is silenced here alone. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wuninitialized"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static int stack_below_holds(const char *needle) {
    volatile unsigned char window[65536];
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= sizeof window; i++) {
        size_t k = 0;
        while (k < n && window[i + k] == (unsigned char)needle[k]) k++;
        if (k == n) return 1;
    }
    return 0;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* Audit F-6c138dc7: rc_client builds the sign-in request ("p=<password>") in a buffer on its
 * own stack (rc_client.c, rc_client_begin_login) and frees it without clearing it. The bridge
 * clears it before rc_client returns, so no copy of the password is left behind. */
static void test_password_is_not_left_on_the_stack(uint8_t *ram) {
    static const char password[] = "Synthetic-Pa55word-91827";
    Fake fake;
    memset(&fake, 0, sizeof fake);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    CHECK(nk_ach_begin_login_with_password(ach, "Player", password), "password sign-in");
    CHECK(requests_holding(&fake, "p=Synthetic-Pa55word-91827") == 1,
          "the sign-in sent the password once");
    CHECK(!stack_below_holds("p=Synthetic-Pa55word-91827"),
          "no copy of the password is left in the stack rc_client used");
    nk_ach_destroy(ach);
}

/* Audit F-282bdd0c: a guest reset. Without nk_ach_reset, the delta trigger kept the 4 it saw
 * before the reset, and the first 5 after it unlocks "Delta" for a change that never happened. */
static void test_reset_forgets_the_old_guest(uint8_t *ram) {
    for (int route = 0; route < 2; route++) {
        Fake fake;
        NkAch *ach = signed_in_with_set(ram, &fake, k_sets_state);
        if (!ach) return;
        for (int i = 0; i < 3; i++) nk_ach_frame(ach);
        ram[ACH_ADDR] = 4;
        nk_ach_frame(ach);
        nk_ach_frame(ach);
        if (route == 1) nk_ach_reset(ach);
        ram[ACH_ADDR] = 5;
        nk_ach_frame(ach);
        const bool unlocked = unlocked_now(ach, 5601u);
        if (route == 0)
            CHECK(unlocked, "control: without a reset the old 4 and the new 5 unlock Delta");
        else
            CHECK(!unlocked, "after nk_ach_reset the old guest's 4 does not unlock Delta");
        nk_ach_destroy(ach);
    }
}

/* Audit F-282bdd0c: a state load. Hit counts saved with a state come back with it: two hits
 * saved, a reset, the state loaded, and one more hit unlocks "Hits"; without the load, the reset
 * trigger waits and nothing unlocks. */
static void test_progress_comes_back_with_a_state(uint8_t *ram) {
    for (int route = 0; route < 2; route++) {
        Fake fake;
        NkAch *ach = signed_in_with_set(ram, &fake, k_sets_state);
        if (!ach) return;
        for (int i = 0; i < 3; i++) nk_ach_frame(ach);
        ram[0x110] = 1;
        nk_ach_frame(ach);
        nk_ach_frame(ach);
        CHECK(!unlocked_now(ach, 5602u), "two hits are not three");
        const size_t size = nk_ach_progress_size(ach);
        uint8_t *state = size ? (uint8_t *)malloc(size) : NULL;
        CHECK(state && nk_ach_serialize_progress(ach, state, size),
              "progress serializes (%u bytes)", (unsigned)size);
        nk_ach_reset(ach);
        if (route == 1)
            CHECK(state && nk_ach_deserialize_progress(ach, state, size), "progress loads");
        nk_ach_frame(ach);
        const bool unlocked = unlocked_now(ach, 5602u);
        if (route == 0)
            CHECK(!unlocked, "control: after a reset alone the trigger starts over");
        else
            CHECK(unlocked, "the loaded state's two hits and one more unlock Hits");
        CHECK(!nk_ach_deserialize_progress(ach, NULL, 0u), "no data is refused");
        free(state);
        nk_ach_destroy(ach);
    }
}

/* "--only <part of a test's name>" runs the tests whose name holds it; no argument runs all. */
#define RUN(test)                                                                            \
    do {                                                                                     \
        if (!only || strstr(#test, only)) test();                                            \
    } while (0)
#define RUN_RAM(test)                                                                        \
    do {                                                                                     \
        if (!only || strstr(#test, only)) test(ram);                                         \
    } while (0)

/* Two unlocks close together are two toasts: the runtime reads the player's messages one at a
 * time, oldest first, so a second message does not replace the first unseen. */
static void test_notifications_are_read_in_order(void) {
    char channel_name[NK_GUEST_MEMORY_CHANNEL_NAME_MAX];
    NkGuestMemoryChannel channel;
    snprintf(channel_name, sizeof channel_name,
             "Local\\NakagawaRecomp.Selftest.GuestMemoryToast.%lu", nk_platform_process_id());
    const bool created = nk_platform_guest_memory_channel_create(&channel, channel_name,
                                                                 CHANNEL_ARENA);
#ifdef _WIN32
    CHECK(created, "the guest-memory channel is created");
#endif
    if (!created) {
        printf("test_nk_achievements: SKIP the notification order: this host has no "
               "guest-memory channel\n");
        return;
    }
    uint32_t seen = 0u;
    char text[NK_GUEST_MEMORY_NOTIFICATION_MAX];
    CHECK(nk_platform_guest_memory_channel_notify(&channel, "Achievement unlocked: First") &&
              nk_platform_guest_memory_channel_notify(&channel, "Achievement unlocked: Second"),
          "two messages in one frame");
    CHECK(nk_platform_guest_memory_channel_read_notification(&channel, &seen, text,
                                                             sizeof text) &&
              strcmp(text, "Achievement unlocked: First") == 0,
          "the first is read first (read \"%s\")", text);
    CHECK(nk_platform_guest_memory_channel_read_notification(&channel, &seen, text,
                                                             sizeof text) &&
              strcmp(text, "Achievement unlocked: Second") == 0,
          "then the second (read \"%s\")", text);
    CHECK(!nk_platform_guest_memory_channel_read_notification(&channel, &seen, text,
                                                              sizeof text),
          "then nothing");

    /* More than the ring holds while the runtime reads none: the newest ones, in order. The
     * oldest slot is the one the player writes next, so a reader that far behind keeps one
     * message fewer than the ring has slots. */
    const unsigned sent = NK_GUEST_MEMORY_NOTIFICATION_RING + 3u;
    for (unsigned i = 0u; i < sent; i++) {
        char message[32];
        snprintf(message, sizeof message, "message %u", i);
        CHECK(nk_platform_guest_memory_channel_notify(&channel, message), "notify %u", i);
    }
    for (unsigned i = sent - (NK_GUEST_MEMORY_NOTIFICATION_RING - 1u); i < sent; i++) {
        char expected[32];
        snprintf(expected, sizeof expected, "message %u", i);
        CHECK(nk_platform_guest_memory_channel_read_notification(&channel, &seen, text,
                                                                 sizeof text) &&
                  strcmp(text, expected) == 0,
              "the ring keeps %s (read \"%s\")", expected, text);
    }
    CHECK(!nk_platform_guest_memory_channel_read_notification(&channel, &seen, text,
                                                              sizeof text),
          "and nothing after the newest");
    nk_platform_guest_memory_channel_close(&channel);
}

/* The player writes message n + 1 into its ring slot before it publishes n + 1, so while the
 * sequence reads n that slot, the slot of message n + 1 - RING, may be half written. A runtime
 * that fell that far behind starts at the next message: a toast is never part of one message
 * and part of another. */
static void test_a_message_being_written_is_not_read(void) {
    char channel_name[NK_GUEST_MEMORY_CHANNEL_NAME_MAX];
    NkGuestMemoryChannel channel;
    snprintf(channel_name, sizeof channel_name,
             "Local\\NakagawaRecomp.Selftest.GuestMemoryTorn.%lu", nk_platform_process_id());
    const bool created = nk_platform_guest_memory_channel_create(&channel, channel_name,
                                                                 CHANNEL_ARENA);
#ifdef _WIN32
    CHECK(created, "the guest-memory channel is created");
#endif
    if (!created) {
        printf("test_nk_achievements: SKIP the message being written: this host has no "
               "guest-memory channel\n");
        return;
    }
    uint32_t seen = 0u;
    char text[NK_GUEST_MEMORY_NOTIFICATION_MAX];
    for (unsigned i = 1u; i <= NK_GUEST_MEMORY_NOTIFICATION_RING; i++) {
        char message[32];
        snprintf(message, sizeof message, "message %u", i);
        CHECK(nk_platform_guest_memory_channel_notify(&channel, message), "notify %u", i);
    }
    /* The player has begun message RING + 1 in the slot of message 1: its first bytes are in,
     * the sequence is not yet raised. */
    memcpy(channel.control->notification_ring[(NK_GUEST_MEMORY_NOTIFICATION_RING + 1u) %
                                              NK_GUEST_MEMORY_NOTIFICATION_RING],
           "TORN", 4u);
    for (unsigned i = 2u; i <= NK_GUEST_MEMORY_NOTIFICATION_RING; i++) {
        char expected[32];
        snprintf(expected, sizeof expected, "message %u", i);
        CHECK(nk_platform_guest_memory_channel_read_notification(&channel, &seen, text,
                                                                 sizeof text) &&
                  strcmp(text, expected) == 0,
              "the slot being written is skipped: %s (read \"%s\")", expected, text);
    }
    CHECK(!nk_platform_guest_memory_channel_read_notification(&channel, &seen, text,
                                                              sizeof text),
          "and nothing after the newest");
    nk_platform_guest_memory_channel_close(&channel);
}

#ifdef _WIN32
/* The section is the game's whole memory and its events pace every frame. Only this user's
 * processes at this integrity level or above may open them: the default descriptor's implicit
 * label forbids a lower level only to write. A "<name>.ack" made by another process first would
 * release or stall the runtime's frames, so a name that already exists is refused. */
static void test_channel_objects_belong_to_this_user(void) {
    char channel_name[NK_GUEST_MEMORY_CHANNEL_NAME_MAX];
    char ack_name[NK_GUEST_MEMORY_CHANNEL_NAME_MAX + 8u];
    NkGuestMemoryChannel channel;
    snprintf(channel_name, sizeof channel_name,
             "Local\\NakagawaRecomp.Selftest.GuestMemorySecurity.%lu", nk_platform_process_id());
    snprintf(ack_name, sizeof ack_name, "%s.ack", channel_name);
    HANDLE squatter = CreateEventA(NULL, FALSE, FALSE, ack_name);
    CHECK(squatter != NULL, "another process makes the acknowledgement event first");
    bool created = nk_platform_guest_memory_channel_create(&channel, channel_name, CHANNEL_ARENA);
    CHECK(!created, "a channel whose event already exists is refused");
    if (created) nk_platform_guest_memory_channel_close(&channel);
    if (squatter) CloseHandle(squatter);

    created = nk_platform_guest_memory_channel_create(&channel, channel_name, CHANNEL_ARENA);
    CHECK(created, "the channel is created once the name is free");
    if (!created) return;
    HANDLE token = NULL;
    union {
        TOKEN_USER user;
        BYTE bytes[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    } user;
    DWORD got = 0u;
    const bool have_user = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                           GetTokenInformation(token, TokenUser, &user, sizeof user, &got);
    if (token) CloseHandle(token);
    CHECK(have_user, "this process's user");
    void *objects[4] = {channel.mapping_handle, channel.frame_event, channel.ack_event,
                        channel.stop_event};
    for (unsigned i = 0u; i < 4u && have_user; i++) {
        PSECURITY_DESCRIPTOR descriptor = NULL;
        PACL dacl = NULL, sacl = NULL;
        const DWORD read = GetSecurityInfo((HANDLE)objects[i], SE_KERNEL_OBJECT,
                                           DACL_SECURITY_INFORMATION |
                                               LABEL_SECURITY_INFORMATION,
                                           NULL, NULL, &dacl, &sacl, &descriptor);
        CHECK(read == ERROR_SUCCESS, "object %u: its descriptor is read", i);
        bool no_read_up = false;
        bool user_only = read == ERROR_SUCCESS && dacl && dacl->AceCount > 0u;
        for (DWORD a = 0u; read == ERROR_SUCCESS && sacl && a < sacl->AceCount; a++) {
            void *ace = NULL;
            if (GetAce(sacl, a, &ace) &&
                ((ACE_HEADER *)ace)->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE &&
                (((SYSTEM_MANDATORY_LABEL_ACE *)ace)->Mask & SYSTEM_MANDATORY_LABEL_NO_READ_UP))
                no_read_up = true;
        }
        for (DWORD a = 0u; user_only && a < dacl->AceCount; a++) {
            void *ace = NULL;
            if (!GetAce(dacl, a, &ace) ||
                ((ACE_HEADER *)ace)->AceType != ACCESS_ALLOWED_ACE_TYPE ||
                !EqualSid((PSID)&((ACCESS_ALLOWED_ACE *)ace)->SidStart, user.user.User.Sid))
                user_only = false;
        }
        CHECK(no_read_up, "object %u: a lower integrity level cannot read it", i);
        CHECK(user_only, "object %u: only this user's processes may open it", i);
        if (descriptor) LocalFree(descriptor);
    }
    nk_platform_guest_memory_channel_close(&channel);
}
#endif

/* An asynchronous transport keeps the request it was handed until it answers: the strings stay
 * valid after rc_client's server_call has returned and its own request buffer is gone. */
static void test_deferred_transport_request_outlives_the_call(uint8_t *ram) {
    static Held d;
    memset(&d, 0, sizeof d);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, held_transport, &d);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    CHECK(d.queued == 1, "one request waits (%d)", d.queued);
    scribble_stack();
    if (d.queued == 1) {
        CHECK(has_prefix_n(d.q[0].url, "https://retroachievements.org/dorequest.php"),
              "the waiting request's URL is intact after server_call returned");
        CHECK(has_prefix_n(d.q[0].post_data, "r=login2&u=Player&"),
              "the waiting request's post data is intact after server_call returned");
    }
    held_pump(&d);
    NkAchEvent ev[8];
    const size_t n = take(ach, ev, 8);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_OK) >= 0, "the late answer signs in");
    nk_ach_destroy(ach);
}

/* nk_ach_destroy while an unlock is still in flight: the late answer reaches a client that is
 * still whole, and the bridge is freed once it arrives (ASan or a crash shows a use after free). */
static void test_destroy_with_a_request_in_flight(uint8_t *ram) {
    static Held d;
    memset(&d, 0, sizeof d);
    d.fake.award_status = 200;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, held_transport, &d);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    nk_ach_begin_login(ach, "Player", "TOKEN");
    held_pump(&d);
    nk_ach_begin_load_game(ach, HASH);
    held_pump(&d);
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(nk_ach_game_loaded(ach), "game loaded");
    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    const int waiting = d.queued - d.answered;
    CHECK(waiting == 1 && has_prefix_n(d.q[d.answered].post_data, "r=awardachievement&"),
          "the award waits for its answer (%d waiting)", waiting);
    nk_ach_destroy(ach);
    /* What the heap held for the client is reused before the late answer arrives. */
    void *churn[64];
    for (int i = 0; i < 64; i++) {
        churn[i] = malloc(64u << (i % 8));
        if (churn[i]) memset(churn[i], 0xa5, 64u << (i % 8));
    }
    held_pump(&d);
    for (int i = 0; i < 64; i++) free(churn[i]);
    CHECK(d.answered == d.queued, "every request was answered");
}

/* A transport that reached no server gives no text: the failure still says why. */
static void test_no_server_failures_name_a_reason(uint8_t *ram) {
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];

    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.login_status = NK_ACH_HTTP_CLIENT_ERROR;
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    nk_ach_begin_login(ach, "Player", "TOKEN");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    int failed = has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED);
    CHECK(failed >= 0 && strstr(ev[failed].message, "no response") != NULL,
          "LOGIN_FAILED names the missing answer (got \"%s\")", failed >= 0 ? ev[failed].message : "");
    nk_ach_destroy(ach);

    ach = nk_ach_create(&mem, silent_transport, NULL);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    nk_ach_begin_login(ach, "Player", "TOKEN");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    failed = has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED);
    CHECK(failed >= 0 && strstr(ev[failed].message, "no response") != NULL,
          "done(NULL) names the missing answer (got \"%s\")", failed >= 0 ? ev[failed].message : "");
    nk_ach_destroy(ach);

    memset(&fake, 0, sizeof fake);
    fake.award_status = NK_ACH_HTTP_CLIENT_ERROR;
    memset(ram, 0, USER_RAM_SIZE);
    ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    nk_ach_begin_login(ach, "Player", "TOKEN");
    nk_ach_begin_load_game(ach, HASH);
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    failed = has_event(ev, n, NK_ACH_EVENT_SERVER_ERROR);
    CHECK(failed >= 0 && ev[failed].message[0] != '\0',
          "an award that reached no server is a SERVER_ERROR with a reason (got \"%s\")",
          failed >= 0 ? ev[failed].message : "");
    nk_ach_destroy(ach);
}

/* NK_ACH_HTTP_RETRY at a token sign-in ends that sign-in as LOGIN_UNREACHABLE: rc_client
 * retries only unlocks and leaderboard submissions, as the header says, so the caller retries the
 * sign-in when nk_ach_token_login_due says so. */
static void test_retry_at_login_is_not_retried(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.login_status = NK_ACH_HTTP_RETRY;
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    nk_ach_begin_login(ach, "Player", "TOKEN");
    for (int i = 0; i < 8; i++) nk_ach_idle(ach);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_UNREACHABLE) >= 0 &&
              has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED) < 0,
          "a retry at sign-in ends the sign-in as unreachable");
    CHECK(fake.requests == 1, "rc_client did not retry the sign-in (%d requests)", fake.requests);
    CHECK(!nk_ach_logged_in(ach), "not logged in");
    nk_ach_destroy(ach);
}

/* An achievement whose trigger this rcheevos cannot parse is reported off. */
static void test_unparsable_achievement_is_reported(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.sets_body = k_sets_parse_error;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    nk_ach_begin_login(ach, "Player", "TOKEN");
    nk_ach_begin_load_game(ach, HASH);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int loaded = has_event(ev, n, NK_ACH_EVENT_GAME_LOADED);
    CHECK(loaded >= 0, "game loads");
    const int disabled = has_event(ev, n, NK_ACH_EVENT_DISABLED);
    CHECK(disabled >= 0, "the unparsable achievement is reported off");
    if (disabled >= 0)
        CHECK(ev[disabled].id == 5503u && strstr(ev[disabled].message, "Parse error") != NULL,
              "DISABLED names 5503 (got %u: %s)", ev[disabled].id, ev[disabled].message);
    nk_ach_destroy(ach);
}

/* The server's set for this hash is for another console: it is not run on PSP memory. */
static void test_set_for_another_console_is_refused(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.sets_body = k_sets_other_console;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    nk_ach_begin_login(ach, "Player", "TOKEN");
    nk_ach_begin_load_game(ach, HASH);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_GAME_LOADED) < 0, "a set for console 12 is not loaded");
    const int failed = has_event(ev, n, NK_ACH_EVENT_GAME_FAILED);
    CHECK(failed >= 0 && strstr(ev[failed].message, "console 12") != NULL,
          "GAME_FAILED names the console (got \"%s\")", failed >= 0 ? ev[failed].message : "");
    CHECK(!nk_ach_game_loaded(ach), "no game loaded");
    ram[ACH_ADDR] = 5;
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKED) < 0, "nothing unlocks");
    CHECK(find_request(&fake, "r=awardachievement&") < 0, "no award is sent");
    nk_ach_destroy(ach);
}

/* A client over the deferred transport, signed in. NULL (with a failure counted) when it cannot be. */
static NkAch *deferred_login(uint8_t *ram, Deferred *d) {
    memset(d, 0, sizeof *d);
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, deferred_transport, d);
    CHECK(ach != NULL, "create");
    if (!ach) return NULL;
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    CHECK(deliver(d, "r=login2&", 200, k_login), "the login request waits for its answer");
    CHECK(nk_ach_logged_in(ach), "logged in");
    return ach;
}

/* rcheevos LOCAL DELTA (rc_client_start_session_callback): the set reply names a game but no set,
 * so the load fails while the session request is still out. When that session reply then carries a
 * server error, rc_client_end_load_state frees the abandoned load; upstream at the pin went on to
 * report the error on the freed load, calling its callback a second time and freeing it again. */
static void test_session_error_after_failed_load(uint8_t *ram) {
    Deferred d;
    NkAch *ach = deferred_login(ram, &d);
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    static const char no_sets[] =
        "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
        "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\",\"Sets\":[]}";
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    CHECK(deliver(&d, "r=achievementsets&", 200, no_sets), "the set request waits for its answer");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(count_events(ev, n, NK_ACH_EVENT_GAME_FAILED) == 1, "a reply with no set fails the load once");
    CHECK(deliver(&d, "r=startsession&", 500, "{\"Success\":false,\"Error\":\"Server failure\"}"),
          "the session request was still out when the load failed");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(count_events(ev, n, NK_ACH_EVENT_GAME_FAILED) == 0,
          "the session error is not reported again on the abandoned load (%d GAME_FAILED)",
          count_events(ev, n, NK_ACH_EVENT_GAME_FAILED));
    CHECK(!nk_ach_game_loaded(ach), "no game loaded");
    CHECK(d.count == 0, "nothing left in flight (%d)", d.count);
    nk_ach_destroy(ach);
}

/* Loads the synthetic game over the deferred transport and earns 5501, leaving its award request
 * waiting. False (with a failure counted) when it cannot. */
static int deferred_unlock(NkAch *ach, uint8_t *ram, Deferred *d) {
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    CHECK(deliver(d, "r=achievementsets&", 200, k_sets), "the set request waits");
    CHECK(deliver(d, "r=startsession&", 200, k_session), "the session request waits");
    CHECK(nk_ach_game_loaded(ach), "game loaded");
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKED) >= 0, "5501 unlocks");
    for (int i = 0; i < d->count; i++)
        if (starts_with(d->q[i].post, "r=awardachievement&")) return 1;
    CHECK(0, "the award request waits for its answer");
    return 0;
}

/* rcheevos LOCAL DELTA (rc_client_award_achievement_callback): the mastering award's answer
 * arrives after the game was unloaded for the next load. Upstream at the pin walked
 * client->game->subsets with client->game NULL. */
static void test_award_reply_after_unload(uint8_t *ram) {
    Deferred d;
    NkAch *ach = deferred_login(ram, &d);
    if (!ach) return;
    if (deferred_unlock(ach, ram, &d)) {
        /* The next load unloads this game at once and leaves its own set request waiting. */
        CHECK(nk_ach_begin_load_game(ach, "fedcba9876543210fedcba9876543210"), "the next load starts");
        CHECK(!nk_ach_game_loaded(ach), "the game is unloaded");
        CHECK(deliver(&d, "r=awardachievement&", 200, k_award),
              "the award request was still out when the game was unloaded");
        CHECK(!nk_ach_game_loaded(ach), "still no game loaded");
    }
    drain(&d);
    nk_ach_destroy(ach);
}

/* rcheevos LOCAL DELTA (rc_client_award_achievement_callback): when the server awards another
 * achievement, the error names both IDs. Upstream at the pin passed error_message (NULL on this
 * path) for the second %u. */
static void test_award_of_another_id_is_logged(uint8_t *ram) {
    Deferred d;
    NkAch *ach = deferred_login(ram, &d);
    if (!ach) return;
    g_log_award[0] = '\0';
    rc_client_enable_logging(nk_ach_rc_client(ach), RC_CLIENT_LOG_LEVEL_WARN, award_log);
    if (deferred_unlock(ach, ram, &d)) {
        static const char other[] =
            "{\"Success\":true,\"Score\":0,\"SoftcoreScore\":17,\"AchievementID\":9999,"
            "\"AchievementsRemaining\":1}";
        CHECK(deliver(&d, "r=awardachievement&", 200, other), "the award request waits");
        CHECK(strcmp(g_log_award, "Awarded achievement 9999 instead of 5501") == 0,
              "the log names the awarded and the requested achievement: \"%s\"", g_log_award);
    }
    drain(&d);
    nk_ach_destroy(ach);
}

/* The synthetic set with achievement 5501's MemAddr and Author replaced. */
static void sets_with(char *out, size_t size, const char *memaddr, const char *author) {
    snprintf(out, size,
             "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
             "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
             "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":\"Core\",\"Type\":\"core\","
             "\"ImageIconUrl\":\"\",\"Achievements\":["
             "{\"ID\":5501,\"Title\":\"Five\",\"Description\":\"The watched byte reads 5\","
             "\"Flags\":3,\"Points\":5,\"MemAddr\":%s,\"Author\":%s,"
             "\"BadgeName\":\"00001\",\"Created\":1700000000,\"Modified\":1700000000}"
             "],\"Leaderboards\":[]}]}",
             memaddr, author);
}

/* rcheevos LOCAL DELTA (rc_json_get_required_string, upstream 8409479c #577): a set whose
 * achievement has "MemAddr": null fails the load. Upstream at the pin took null as a present
 * required string, and activating the set passed the NULL definition to strlen. An achievement
 * whose author is null (a deleted user) still loads, as upstream #577 keeps it. */
static void test_null_required_string(uint8_t *ram) {
    char body[1024];
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    Deferred d;
    NkAch *ach = deferred_login(ram, &d);
    if (!ach) return;
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    sets_with(body, sizeof body, "null", "\"Synthetic\"");
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    CHECK(deliver(&d, "r=achievementsets&", 200, body), "the set request waits for its answer");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int failed = has_event(ev, n, NK_ACH_EVENT_GAME_FAILED);
    CHECK(failed >= 0, "a set with \"MemAddr\": null fails the load");
    if (failed >= 0)
        CHECK(strstr(ev[failed].message, "MemAddr") != NULL, "the error names MemAddr: %s",
              ev[failed].message);
    CHECK(!nk_ach_game_loaded(ach), "no game loaded");
    drain(&d);
    nk_ach_destroy(ach);

    ach = deferred_login(ram, &d);
    if (!ach) return;
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    sets_with(body, sizeof body, "\"0xH800100=5\"", "null");
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    CHECK(deliver(&d, "r=achievementsets&", 200, body), "the set request waits for its answer");
    CHECK(deliver(&d, "r=startsession&", 200, k_session), "the session request waits");
    CHECK(nk_ach_game_loaded(ach), "an achievement whose author is null still loads");
    drain(&d);
    nk_ach_destroy(ach);
}

/* A client over the deferred transport whose login request waits for its answer. */
static NkAch *deferred_login_waiting(uint8_t *ram, Deferred *d) {
    memset(d, 0, sizeof *d);
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, deferred_transport, d);
    CHECK(ach != NULL, "create");
    if (!ach) return NULL;
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    return ach;
}

/* rcheevos LOCAL DELTA (rc_json_parse_field, upstream 8409479c #577): a reply nested deeper than
 * 32 arrays is refused as invalid JSON. Upstream at the pin recursed once per '[' with no limit,
 * so a reply of a million '[' overflowed the native stack. */
static void test_deep_json_is_refused(uint8_t *ram) {
    static const char prefix[] = "{\"Success\":true,\"User\":";
    const size_t depth = (size_t)1u << 20;
    const size_t len = sizeof prefix - 1u + depth;
    char *body = (char *)malloc(len + 1u);
    CHECK(body != NULL, "out of memory");
    if (!body) return;
    memcpy(body, prefix, sizeof prefix - 1u);
    memset(body + sizeof prefix - 1u, '[', depth);
    body[len] = '\0';
    Deferred d;
    NkAch *ach = deferred_login_waiting(ram, &d);
    if (ach) {
        CHECK(deliver_n(&d, "r=login2&", 200, body, len), "the login request waits for its answer");
        NkAchEvent ev[NK_ACH_EVENT_QUEUE];
        const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
        CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED) >= 0, "a reply nested 2^20 deep fails the login");
        CHECK(!nk_ach_logged_in(ach), "not logged in");
        drain(&d);
        nk_ach_destroy(ach);
    }
    free(body);
}

/* rcheevos LOCAL DELTA (rc_json_parse_server_response): an answer with body_length 0 is an empty
 * body whatever its pointer points at. Upstream at the pin read body[0] past the length, so a 504
 * whose zero-length body pointed at a non-NUL byte lost its "Request has timed out." message. */
static void test_empty_body_reads_nothing(uint8_t *ram) {
    static const char after[] = "x";
    Deferred d;
    NkAch *ach = deferred_login_waiting(ram, &d);
    if (!ach) return;
    CHECK(deliver_n(&d, "r=login2&", 504, after, 0u), "the login request waits for its answer");
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    /* A 504 is no answer from the server: the token sign-in is unreachable, not refused. */
    const int failed = has_event(ev, n, NK_ACH_EVENT_LOGIN_UNREACHABLE);
    CHECK(failed >= 0, "a 504 with no body ends the login as unreachable");
    if (failed >= 0)
        CHECK(strstr(ev[failed].message, "Request has timed out.") != NULL,
              "the empty 504 reads as a timeout: \"%s\"", ev[failed].message);
    drain(&d);
    nk_ach_destroy(ach);
}

/* ---- arrays that hold a non-object (rcheevos LOCAL DELTA, upstream c0a3ebdd #553) ---------- */

static rc_api_server_response_t reply(const char *body) {
    rc_api_server_response_t r;
    memset(&r, 0, sizeof r);
    r.body = body;
    r.body_length = strlen(body);
    r.http_status_code = 200;
    return r;
}

#define SETS_HEAD                                                                              \
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"         \
    "\"ImageIconUrl\":\"\",\"Sets\":"
#define SET_HEAD                                                                               \
    "[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":\"Core\",\"Type\":\"core\","        \
    "\"ImageIconUrl\":\"\","
#define ONE_ACH                                                                                \
    "{\"ID\":5501,\"Title\":\"Five\",\"Description\":\"The watched byte reads 5\",\"Flags\":3," \
    "\"Points\":5,\"MemAddr\":\"0xH800100=5\",\"Author\":\"Synthetic\",\"BadgeName\":\"00001\","  \
    "\"Created\":1700000000,\"Modified\":1700000000}"
#define ONE_LB                                                                                 \
    "{\"ID\":7701,\"Title\":\"Score\",\"Description\":\"The watched byte\","                  \
    "\"Mem\":\"STA:0xH800100=1::CAN:0xH800100=2::SUB:0xH800100=3::VAL:0xH800100\","             \
    "\"Format\":\"VALUE\"}"

/* The rapi parsers count every element of an array, but fill only the objects up to the first
 * entry that is not one. Upstream at the pin returned RC_OK with the rest of the count left as
 * uninitialized memory, which rc_client then read as pointers (rc_client_copy_achievements). Each
 * parser rc_client calls on such a reply now refuses it, with the count cut to what was filled. */
static void test_array_of_primitives_is_refused(void) {
    rc_api_server_response_t r;
    int res;

    rc_api_fetch_game_sets_response_t sets;
    r = reply(SETS_HEAD SET_HEAD "\"Achievements\":[" ONE_ACH "],\"Leaderboards\":[" ONE_LB "]}]}");
    res = rc_api_process_fetch_game_sets_server_response(&sets, &r);
    CHECK(res == RC_OK && sets.num_sets == 1 && sets.sets[0].num_achievements == 1 &&
              sets.sets[0].num_leaderboards == 1,
          "a well-formed set still loads (%d)", res);
    rc_api_destroy_fetch_game_sets_response(&sets);

    r = reply(SETS_HEAD "[1]}");
    res = rc_api_process_fetch_game_sets_server_response(&sets, &r);
    CHECK(res == RC_INVALID_VALUE && sets.num_sets == 0, "\"Sets\":[1] is refused (%d, %u sets)", res,
          (unsigned)sets.num_sets);
    rc_api_destroy_fetch_game_sets_response(&sets);

    r = reply(SETS_HEAD SET_HEAD "\"Achievements\":[" ONE_ACH ",1],\"Leaderboards\":[]}]}");
    res = rc_api_process_fetch_game_sets_server_response(&sets, &r);
    CHECK(res == RC_INVALID_VALUE && sets.sets[0].num_achievements == 1,
          "\"Achievements\":[{...},1] is refused (%d)", res);
    rc_api_destroy_fetch_game_sets_response(&sets);

    r = reply(SETS_HEAD SET_HEAD "\"Achievements\":[],\"Leaderboards\":[" ONE_LB ",1]}]}");
    res = rc_api_process_fetch_game_sets_server_response(&sets, &r);
    CHECK(res == RC_INVALID_VALUE && sets.sets[0].num_leaderboards == 1,
          "\"Leaderboards\":[{...},1] is refused (%d)", res);
    rc_api_destroy_fetch_game_sets_response(&sets);

    rc_api_start_session_response_t session;
    r = reply("{\"Success\":true,\"HardcoreUnlocks\":[111,112,113],"
              "\"Unlocks\":[{\"ID\":114,\"When\":1234567840}],\"ServerNow\":1234577777}");
    res = rc_api_process_start_session_server_response(&session, &r);
    CHECK(res == RC_INVALID_VALUE && session.num_unlocks == 1 && session.num_hardcore_unlocks == 0,
          "\"HardcoreUnlocks\":[111,...] is refused (%d, %u hardcore)", res,
          (unsigned)session.num_hardcore_unlocks);
    rc_api_destroy_start_session_response(&session);

    r = reply("{\"Success\":true,\"Unlocks\":[{\"ID\":114,\"When\":1234567840},7]}");
    res = rc_api_process_start_session_server_response(&session, &r);
    CHECK(res == RC_INVALID_VALUE && session.num_unlocks == 1, "\"Unlocks\":[{...},7] is refused (%d)", res);
    rc_api_destroy_start_session_response(&session);

    rc_api_submit_lboard_entry_response_t lboard;
    r = reply("{\"Success\":true,\"Response\":{\"Score\":5,\"BestScore\":5,"
              "\"RankInfo\":{\"Rank\":1,\"NumEntries\":\"2\"},"
              "\"TopEntries\":[{\"User\":\"Player\",\"Rank\":1,\"Score\":5},1]}}");
    res = rc_api_process_submit_lboard_entry_server_response(&lboard, &r);
    CHECK(res == RC_INVALID_VALUE && lboard.num_top_entries == 1, "\"TopEntries\":[{...},1] is refused (%d)",
          res);
    rc_api_destroy_submit_lboard_entry_response(&lboard);

    rc_api_fetch_followed_users_response_t users;
    r = reply("{\"Success\":true,\"Friends\":[1]}");
    res = rc_api_process_fetch_followed_users_server_response(&users, &r);
    CHECK(res == RC_INVALID_VALUE && users.num_users == 0, "\"Friends\":[1] is refused (%d)", res);
    rc_api_destroy_fetch_followed_users_response(&users);

    rc_api_fetch_games_list_response_t games;
    r = reply("{\"Success\":true,\"Response\":[1]}");
    res = rc_api_process_fetch_games_list_server_response(&games, &r);
    CHECK(res == RC_INVALID_VALUE && games.num_entries == 0, "a games list of [1] is refused (%d)", res);
    rc_api_destroy_fetch_games_list_response(&games);

    rc_api_fetch_game_titles_response_t titles;
    res = rc_api_process_fetch_game_titles_server_response(&titles, &r);
    CHECK(res == RC_INVALID_VALUE && titles.num_entries == 0, "a game title list of [1] is refused (%d)", res);
    rc_api_destroy_fetch_game_titles_response(&titles);

    rc_api_fetch_leaderboard_info_response_t info;
    r = reply("{\"Success\":true,\"LeaderboardData\":{\"LBID\":7701,\"LBFormat\":\"VALUE\","
              "\"LowerIsBetter\":0,\"LBTitle\":\"Score\",\"LBDesc\":\"d\",\"LBMem\":\"m\",\"GameID\":1234,"
              "\"LBAuthor\":\"Synthetic\",\"LBCreated\":\"2023-11-14 22:13:20\","
              "\"LBUpdated\":\"2023-11-14 22:13:20\",\"TotalEntries\":1,\"Entries\":[1]}}");
    res = rc_api_process_fetch_leaderboard_info_server_response(&info, &r);
    CHECK(res == RC_INVALID_VALUE && info.num_entries == 0, "leaderboard \"Entries\":[1] is refused (%d)", res);
    rc_api_destroy_fetch_leaderboard_info_response(&info);

    rc_api_fetch_achievement_info_response_t ach_info;
    r = reply("{\"Success\":true,\"AchievementID\":5501,\"Response\":{\"NumEarned\":1,"
              "\"TotalPlayers\":1,\"GameID\":1234,\"RecentWinner\":[1]}}");
    res = rc_api_process_fetch_achievement_info_server_response(&ach_info, &r);
    CHECK(res == RC_INVALID_VALUE && ach_info.num_recently_awarded == 0,
          "\"RecentWinner\":[1] is refused (%d)", res);
    rc_api_destroy_fetch_achievement_info_response(&ach_info);
}

/* The same through the bridge: a set reply whose achievement array holds a number fails the load
 * by name. Upstream at the pin loaded it and copied the second achievement from uninitialized
 * memory. */
static void test_set_with_primitive_entry_fails_load(uint8_t *ram) {
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    Deferred d;
    NkAch *ach = deferred_login(ram, &d);
    if (!ach) return;
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    CHECK(deliver(&d, "r=achievementsets&", 200,
                  SETS_HEAD SET_HEAD "\"Achievements\":[" ONE_ACH ",1],\"Leaderboards\":[]}]}"),
          "the set request waits for its answer");
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_GAME_FAILED) >= 0, "a set whose achievements hold a number fails the load");
    CHECK(!nk_ach_game_loaded(ach), "no game loaded");
    drain(&d);
    nk_ach_destroy(ach);
}

/* rcheevos LOCAL DELTA (src/rcheevos/alloc.c): an achievement that watches more distinct addresses
 * than one memref list holds. A list's capacity is 16 bits; upstream at the pin sized the new list
 * to the next power of two, so 32769 to 65536 new memrefs truncated it to 0, and the parse then
 * put every memref in the trigger's buffer past the size it had measured (a heap overflow, which
 * AddressSanitizer reports on the upstream bytes). The set must load and the achievement must
 * still unlock when every watched byte reads 5. */
#define MANY_MEMREFS 40000u
#define MANY_BASE 0x10000u /* user RAM offset of the first watched byte */
static void test_achievement_with_many_memrefs_loads(uint8_t *ram) {
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    static const char ach_head[] =
        "{\"ID\":5501,\"Title\":\"Many\",\"Description\":\"Every watched byte reads 5\",\"Flags\":3,"
        "\"Points\":5,\"MemAddr\":\"";
    static const char ach_tail[] =
        "\",\"Author\":\"Synthetic\",\"BadgeName\":\"00001\",\"Created\":1700000000,"
        "\"Modified\":1700000000}],\"Leaderboards\":[]}]}";
    const size_t cap = sizeof(SETS_HEAD SET_HEAD "\"Achievements\":[") + sizeof ach_head +
                       sizeof ach_tail + (size_t)MANY_MEMREFS * 16u;
    char *body = (char *)malloc(cap);
    CHECK(body != NULL, "out of memory");
    if (!body) return;
    size_t len = (size_t)snprintf(body, cap, "%s%s", SETS_HEAD SET_HEAD "\"Achievements\":[", ach_head);
    for (uint32_t i = 0; i < MANY_MEMREFS; i++)
        len += (size_t)snprintf(body + len, cap - len, "%s0xH%06x=5", i ? "_" : "",
                                (unsigned)(0x800000u + MANY_BASE + i));
    len += (size_t)snprintf(body + len, cap - len, "%s", ach_tail);
    CHECK(len < cap, "the set reply fits its buffer");

    Deferred d;
    NkAch *ach = deferred_login(ram, &d);
    if (!ach) {
        free(body);
        return;
    }
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    CHECK(deliver_n(&d, "r=achievementsets&", 200, body, len), "the set request waits for its answer");
    CHECK(deliver(&d, "r=startsession&", 200, k_session), "the session request waits");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int loaded = has_event(ev, n, NK_ACH_EVENT_GAME_LOADED);
    CHECK(loaded >= 0 && ev[loaded].count == 1u, "the set of one achievement over %u addresses loads",
          MANY_MEMREFS);
    CHECK(has_event(ev, n, NK_ACH_EVENT_DISABLED) < 0, "the achievement is not turned off");

    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKED) < 0, "no unlock while the bytes read 0");
    memset(ram + MANY_BASE, 5, MANY_MEMREFS);
    nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int unlocked = has_event(ev, n, NK_ACH_EVENT_UNLOCKED);
    CHECK(unlocked >= 0 && ev[unlocked].id == 5501u, "the achievement unlocks when all %u bytes read 5",
          MANY_MEMREFS);
    drain(&d);
    nk_ach_destroy(ach);
    free(body);
}

/* Loads the synthetic set with RichPresencePatch set to script (already escaped for JSON) on a
 * signed-in client over the deferred transport. NULL (with a failure counted) when it does not
 * load. */
static NkAch *load_with_rich_presence(uint8_t *ram, Deferred *d, const char *script) {
    static const char head[] =
        "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
        "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"";
    static const char tail[] =
        "\",\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":\"Core\",\"Type\":\"core\","
        "\"ImageIconUrl\":\"\",\"Achievements\":["
        "{\"ID\":5501,\"Title\":\"Five\",\"Description\":\"The watched byte reads 5\","
        "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800100=5\",\"Author\":\"Synthetic\","
        "\"BadgeName\":\"00001\",\"Created\":1700000000,\"Modified\":1700000000}"
        "],\"Leaderboards\":[]}]}";
    const size_t len = sizeof head - 1u + strlen(script) + sizeof tail - 1u;
    char *body = (char *)malloc(len + 1u);
    CHECK(body != NULL, "out of memory");
    if (!body) return NULL;
    snprintf(body, len + 1u, "%s%s%s", head, script, tail);
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    NkAch *ach = deferred_login(ram, d);
    if (ach) {
        (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
        CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
        CHECK(deliver_n(d, "r=achievementsets&", 200, body, len), "the set request waits for its answer");
        CHECK(deliver(d, "r=startsession&", 200, k_session), "the session request waits");
        CHECK(nk_ach_game_loaded(ach), "the set with a rich presence script loads");
        if (!nk_ach_game_loaded(ach)) {
            drain(d);
            nk_ach_destroy(ach);
            ach = NULL;
        }
    }
    free(body);
    return ach;
}

/* rcheevos LOCAL DELTA (rc_evaluate_richpresence_display, upstream cab7f7ff #569): an unknown
 * macro's text is the whole "Name(params)" span, so "[Unknown macro]Name(params)" can be longer
 * than the 256-byte tmp it is formatted into. Upstream at the pin took snprintf's untruncated
 * length as the copy length, so a caller buffer larger than tmp received the stack bytes past tmp
 * and a length longer than the text. */
static void test_long_unknown_macro_is_cut(uint8_t *ram) {
    enum { PARAMS = 600 };
    char script[64 + PARAMS];
    int n = snprintf(script, sizeof script, "Display:\\n@Unknown(");
    memset(script + n, 'p', PARAMS);
    snprintf(script + n + PARAMS, sizeof script - (size_t)n - PARAMS, ")");
    Deferred d;
    NkAch *ach = load_with_rich_presence(ram, &d, script);
    if (!ach) return;
    char message[1024];
    memset(message, 'x', sizeof message);
    const size_t got = rc_client_get_rich_presence_message(nk_ach_rc_client(ach), message, sizeof message);
    CHECK(got == 255u, "the unknown macro's text is cut to 255 chars, got %u", (unsigned)got);
    CHECK(memchr(message, '\0', sizeof message) == message + 255, "the message ends after 255 chars");
    CHECK(starts_with(message, "[Unknown macro]Unknown(ppp"), "the message is the unknown macro: %.32s", message);
    int untouched = 1;
    for (size_t i = 256u; i < sizeof message; i++)
        if (message[i] != 'x') untouched = 0;
    CHECK(untouched, "nothing is written past the cut text (no bytes read past the 256-byte tmp)");
    drain(&d);
    nk_ach_destroy(ach);
}

/* rcheevos LOCAL DELTA (rc_parse_richpresence_lookup, upstream cab7f7ff #569): lookups are
 * normally listed in value order. Upstream at the pin inserted each entry into a plain binary
 * tree from its root, so sorted entries built a list: quadratic time to parse, then a count and a
 * flatten that recursed once per entry: 65536 entries overflowed the 2 MB main-thread stack of a
 * Windows build. The entries are now kept in a sorted array and the balanced tree built from it
 * once. The keys are even, so no two entries merge into one range, and the labels repeat (L0 to
 * L6) so the parser's string table stays small. */
#define SORTED_LOOKUP_ENTRIES (1u << 16)
#define RP_ADDR 0x00200u /* user RAM offset the rich presence reads (32 bits) */
static void test_long_sorted_lookup_loads(uint8_t *ram) {
    static const char head[] = "Lookup:L\\n*=odd\\n";
    static const char tail[] = "\\nDisplay:\\n@L(0xX800200)";
    const size_t cap = sizeof head + sizeof tail + (size_t)SORTED_LOOKUP_ENTRIES * 16u;
    char *script = (char *)malloc(cap);
    CHECK(script != NULL, "out of memory");
    if (!script) return;
    size_t len = (size_t)snprintf(script, cap, "%s", head);
    for (uint32_t i = 0; i < SORTED_LOOKUP_ENTRIES; i++)
        len += (size_t)snprintf(script + len, cap - len, "%u=L%u\\n", (unsigned)(2u * i), (unsigned)(i % 7u));
    len += (size_t)snprintf(script + len, cap - len, "%s", tail);
    CHECK(len < cap, "the script fits its buffer");
    Deferred d;
    NkAch *ach = load_with_rich_presence(ram, &d, script);
    free(script);
    if (!ach) return;
    const uint32_t values[4] = {0u, 80000u, 2u * (SORTED_LOOKUP_ENTRIES - 1u), 80001u};
    for (int i = 0; i < 4; i++) {
        char want[16], message[64];
        ram[RP_ADDR + 0] = (uint8_t)values[i];
        ram[RP_ADDR + 1] = (uint8_t)(values[i] >> 8);
        ram[RP_ADDR + 2] = (uint8_t)(values[i] >> 16);
        ram[RP_ADDR + 3] = (uint8_t)(values[i] >> 24);
        nk_ach_frame(ach);
        if (values[i] & 1u)
            snprintf(want, sizeof want, "odd");
        else
            snprintf(want, sizeof want, "L%u", (unsigned)(values[i] / 2u % 7u));
        rc_client_get_rich_presence_message(nk_ach_rc_client(ach), message, sizeof message);
        CHECK(strcmp(message, want) == 0, "the lookup of %u reads %s, got %s", (unsigned)values[i], want,
              message);
    }
    drain(&d);
    nk_ach_destroy(ach);
}

/* rcheevos LOCAL DELTA (rc_runtime_deserialize_progress_sized, upstream e9a6d580 #554): a chunk's
 * size is checked against the bytes left, so it cannot wrap the 32-bit chunk end back inside the
 * state. Upstream at the pin added the size to the offset unchecked: a DONE chunk whose size wraps
 * passed the bounds test, and its checksum was then read from the 15 bytes past the state's stated
 * size, so a state accepted only by reading past its end loaded as RC_OK. The bytes past the
 * stated size here are that checksum, so the over-read is visible as an accepted state. */
static void test_progress_chunk_cannot_wrap(uint8_t *ram) {
    Deferred d;
    NkAch *ach = deferred_login(ram, &d);
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    CHECK(deliver(&d, "r=achievementsets&", 200, k_sets), "the set request waits for its answer");
    CHECK(deliver(&d, "r=startsession&", 200, k_session), "the session request waits");
    CHECK(nk_ach_game_loaded(ach), "the game loads");
    /* "RAP\n", an unknown 8-byte chunk "PADD", then DONE whose size 0xFFFFFFF0 wraps the chunk
     * end from 28 back to 12. The stated size, 29, ends one byte into the checksum. */
    static const uint8_t prefix[28] = {
        0x52, 0x41, 0x50, 0x0A, 'P', 'A', 'D', 'D', 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        'D', 'O', 'N', 'E', 0xF0, 0xFF, 0xFF, 0xFF};
    uint8_t state[28 + 16];
    memcpy(state, prefix, sizeof prefix);
    md5_state_t md5;
    md5_init(&md5);
    md5_append(&md5, state, (int)sizeof prefix);
    md5_finish(&md5, state + sizeof prefix);
    const int result = rc_client_deserialize_progress_sized(nk_ach_rc_client(ach), state, sizeof prefix + 1u);
    CHECK(result == RC_INSUFFICIENT_BUFFER, "a chunk size that wraps past the state's end is refused, got %d",
          result);
    drain(&d);
    nk_ach_destroy(ach);
}

typedef struct LbEntries {
    int calls;
    int result;
    uint32_t num_entries;
    int32_t user_index;
} LbEntries;

static void RC_CCONV lb_entries_done(int result, const char *error_message, rc_client_leaderboard_entry_list_t *list,
                                     rc_client_t *client, void *userdata) {
    LbEntries *out = (LbEntries *)userdata;
    (void)error_message;
    (void)client;
    out->calls++;
    out->result = result;
    if (list) {
        out->num_entries = list->num_entries;
        out->user_index = list->user_index;
        rc_client_destroy_leaderboard_entry_list(list);
    }
}

/* rcheevos LOCAL DELTA (rc_client_fetch_leaderboard_entries_callback, upstream db8ca2e6 #570):
 * leaderboard entries can be fetched before anyone signs in, and the reply then looks for the
 * signed-in user's entry. Upstream at the pin passed the NULL display name to strlen. */
static void test_leaderboard_entries_without_login(uint8_t *ram) {
    Deferred d;
    memset(&d, 0, sizeof d);
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, deferred_transport, &d);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    static const char lbinfo[] =
        "{\"Success\":true,\"LeaderboardData\":{\"LBID\":7001,\"LBFormat\":\"VALUE\",\"LowerIsBetter\":0,"
        "\"LBTitle\":\"Synthetic Board\",\"LBDesc\":\"A synthetic leaderboard\",\"LBMem\":\"\","
        "\"GameID\":1234,\"LBAuthor\":\"Synthetic\",\"LBCreated\":\"2026-01-01 00:00:00\","
        "\"LBUpdated\":\"2026-01-01 00:00:00\",\"Entries\":[{\"User\":\"Someone\",\"Rank\":1,"
        "\"Index\":1,\"Score\":100,\"DateSubmitted\":1700000000}],\"TotalEntries\":1}}";
    LbEntries out = {0, -1, 0u, 0};
    (void)rc_client_begin_fetch_leaderboard_entries(nk_ach_rc_client(ach), 7001u, 1u, 10u, lb_entries_done, &out);
    CHECK(deliver(&d, "r=lbinfo&", 200, lbinfo), "the leaderboard request waits for its answer");
    CHECK(out.calls == 1 && out.result == RC_OK, "the entries arrive once, got %d call(s), result %d", out.calls,
          out.result);
    CHECK(out.num_entries == 1u && out.user_index == -1,
          "one entry and no signed-in user's entry, got %u entries, user index %d", (unsigned)out.num_entries,
          (int)out.user_index);
    drain(&d);
    nk_ach_destroy(ach);
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--runtime-child") == 0)
        return runtime_child(argv[2], argv[3]);
    const char *only = argc == 3 && strcmp(argv[1], "--only") == 0 ? argv[2] : NULL;
    uint8_t *ram = (uint8_t *)calloc(1u, USER_RAM_SIZE);
    if (!ram) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    RUN(test_url_gate);
    RUN(test_psp_disc_hash);
    RUN_RAM(test_memory_map);
    RUN_RAM(test_other_host_is_refused);
    RUN_RAM(test_no_transport_sends_nothing);
    RUN_RAM(test_login_load_unlock);
    RUN_RAM(test_award_offline_is_pending);
    RUN_RAM(test_sign_out_discards_saved_unlocks);
    RUN_RAM(test_refused_saved_unlock_does_not_hold_the_queue);
    RUN_RAM(test_saved_unlock_waits_for_the_server_and_the_account);
    RUN_RAM(test_saved_unlocks_go_only_to_their_account);
    RUN_RAM(test_queue_file_names_its_account);
    RUN_RAM(test_old_queue_file_is_set_aside);
    RUN_RAM(test_unreadable_queue_file_is_kept);
    RUN_RAM(test_token_sign_in_is_tried_again_when_the_server_returns);
    RUN_RAM(test_every_entry_takes_the_lock);
    RUN_RAM(test_last_achievement_answered_as_the_game_ends);
    RUN_RAM(test_award_retries_end_with_their_account);
    RUN_RAM(test_award_retry_keeps_its_game);
    RUN_RAM(test_refused_live_award_leaves_the_queue);
    RUN_RAM(test_late_answer_after_sign_out_keeps_the_next_unlock);
    RUN_RAM(test_requests_wait_for_the_one_transport_slot);
    RUN_RAM(test_requests_waiting_at_sign_out_are_not_sent);
    RUN_RAM(test_late_replay_answer_leaves_the_next_account);
    RUN_RAM(test_sign_in_ended_by_logout_is_not_tried_again);
    RUN_RAM(test_long_titles_are_cut_at_a_character);
    RUN(test_frames_are_served_while_the_event_loop_is_stalled);
    RUN(test_notifications_are_read_in_order);
    RUN(test_a_message_being_written_is_not_read);
#ifdef _WIN32
    RUN(test_channel_objects_belong_to_this_user);
#endif
    RUN_RAM(test_deferred_transport_request_outlives_the_call);
    RUN_RAM(test_destroy_with_a_request_in_flight);
    RUN_RAM(test_no_server_failures_name_a_reason);
    RUN_RAM(test_retry_at_login_is_not_retried);
    RUN_RAM(test_unparsable_achievement_is_reported);
    RUN_RAM(test_set_for_another_console_is_refused);
    RUN_RAM(test_award_of_another_id_is_logged);
    RUN_RAM(test_session_error_after_failed_load);
    RUN_RAM(test_award_reply_after_unload);
    RUN_RAM(test_null_required_string);
    RUN_RAM(test_empty_body_reads_nothing);
    RUN_RAM(test_deep_json_is_refused);
    RUN(test_array_of_primitives_is_refused);
    RUN_RAM(test_set_with_primitive_entry_fails_load);
    RUN_RAM(test_achievement_with_many_memrefs_loads);
    RUN_RAM(test_long_unknown_macro_is_cut);
    RUN_RAM(test_long_sorted_lookup_loads);
    RUN_RAM(test_progress_chunk_cannot_wrap);
    RUN_RAM(test_hardcore_cannot_be_turned_on);
    RUN_RAM(test_switched_off_sends_nothing);
    RUN(test_user_agent_names_this_player);
    RUN_RAM(test_password_is_not_left_on_the_stack);
    RUN_RAM(test_reset_forgets_the_old_guest);
    RUN_RAM(test_progress_comes_back_with_a_state);
    RUN_RAM(test_leaderboard_entries_without_login);
    free(ram);
    if (g_failures) {
        fprintf(stderr, "test_nk_achievements: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_achievements: PASS\n");
    return 0;
}
