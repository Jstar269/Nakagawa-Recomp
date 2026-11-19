// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The RetroAchievements bridge (src/core/nk_achievements.c, #1316) on the vendored rcheevos
 * rc_client, against an in-process fake server: no socket, no network (AGENTS.md section 5).
 * The fake answers in the response shapes rcheevos' parsers read (src/rapi/rc_api_user.c,
 * src/rapi/rc_api_runtime.c); the game, achievement and user are synthetic. */

#include "nk_achievements.h"

#include "rc_api_info.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"

/* rhash's MD5, which signs a progress state's DONE chunk; vendored, not a public header. */
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

#define USER_RAM 0x08800000u
#define USER_RAM_SIZE 0x01800000u /* 24 MB */
#define HASH "0123456789abcdef0123456789abcdef"
#define ACH_ADDR 0x00100u           /* user RAM offset the synthetic achievement watches */

/* ---- the fake server ------------------------------------------------------------------ */

typedef struct Fake {
    int requests;
    char log[16][512];       /* the post data of each request, in order */
    int award_status;        /* the HTTP status awardachievement answers */
    int login_status;        /* the HTTP status login2 answers; 0 means 200 */
    const char *sets;        /* the achievementsets answer; NULL means k_sets */
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

/* The fake server's answer to one request's post data, logged in order. */
static NkAchResponse fake_answer(Fake *f, const char *post_data) {
    const char *post = post_data ? post_data : "";
    if (f->requests < 16) snprintf(f->log[f->requests], sizeof f->log[0], "%s", post);
    f->requests++;
    NkAchResponse r = {k_ok, sizeof k_ok - 1u, 200};
    if (starts_with(post, "r=login2&")) {
        r.body = k_login;
        if (f->login_status) r.http_status = f->login_status;
        if (f->login_status < 0) r.body = "";
    }
    else if (starts_with(post, "r=achievementsets&")) r.body = f->sets ? f->sets : k_sets;
    else if (starts_with(post, "r=startsession&")) r.body = k_session;
    else if (starts_with(post, "r=awardachievement&")) {
        r.body = k_award;
        r.http_status = f->award_status;
        if (f->award_status < 0) r.body = "";
    }
    r.body_length = strlen(r.body);
    return r;
}

static void fake_transport(void *ctx, const NkAchRequest *req, NkAchDone done, void *done_ctx) {
    NkAchResponse r = fake_answer((Fake *)ctx, req->post_data);
    done(&r, done_ctx);
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
    nk_ach_destroy(ach);
}

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

/* NK_ACH_HTTP_RETRY at sign-in is a final LOGIN_FAILED: rc_client retries only unlocks and
 * leaderboard submissions, as the header says, so the caller retries the sign-in. */
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
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED) >= 0, "a retry at sign-in fails the sign-in");
    CHECK(fake.requests == 1, "rc_client did not retry the sign-in (%d requests)", fake.requests);
    CHECK(!nk_ach_logged_in(ach), "not logged in");
    nk_ach_destroy(ach);
}

/* An achievement whose trigger this rcheevos cannot parse is reported off. */
static void test_unparsable_achievement_is_reported(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    fake.sets = k_sets_parse_error;
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
    fake.sets = k_sets_other_console;
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
    const int failed = has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED);
    CHECK(failed >= 0, "a 504 with no body fails the login");
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

int main(void) {
    uint8_t *ram = (uint8_t *)calloc(1u, USER_RAM_SIZE);
    if (!ram) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    test_url_gate();
    test_memory_map(ram);
    test_other_host_is_refused(ram);
    test_no_transport_sends_nothing(ram);
    test_login_load_unlock(ram);
    test_award_offline_is_pending(ram);
    test_deferred_transport_request_outlives_the_call(ram);
    test_destroy_with_a_request_in_flight(ram);
    test_no_server_failures_name_a_reason(ram);
    test_retry_at_login_is_not_retried(ram);
    test_unparsable_achievement_is_reported(ram);
    test_set_for_another_console_is_refused(ram);
    test_award_of_another_id_is_logged(ram);
    test_session_error_after_failed_load(ram);
    test_award_reply_after_unload(ram);
    test_null_required_string(ram);
    test_empty_body_reads_nothing(ram);
    test_deep_json_is_refused(ram);
    test_array_of_primitives_is_refused();
    test_set_with_primitive_entry_fails_load(ram);
    test_achievement_with_many_memrefs_loads(ram);
    test_long_unknown_macro_is_cut(ram);
    test_long_sorted_lookup_loads(ram);
    test_progress_chunk_cannot_wrap(ram);
    free(ram);
    if (g_failures) {
        fprintf(stderr, "test_nk_achievements: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_achievements: PASS\n");
    return 0;
}
