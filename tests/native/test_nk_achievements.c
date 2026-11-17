// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The RetroAchievements bridge (src/core/nk_achievements.c, #1316) on the vendored rcheevos
 * rc_client, against an in-process fake server: no socket, no network (AGENTS.md section 5).
 * The fake answers in the response shapes rcheevos' parsers read (src/rapi/rc_api_user.c,
 * src/rapi/rc_api_runtime.c); the game, achievement and user are synthetic. */

#include "nk_achievements.h"

#include "rc_client.h"
#include "rc_consoles.h"

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
        r.body = k_award;
        r.http_status = f->award_status;
        if (f->award_status < 0) r.body = "";
    }
    r.body_length = strlen(r.body);
    done(&r, done_ctx);
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

/* Answers the oldest waiting request whose post data starts with prefix. False when none waits. */
static int deliver(Deferred *d, const char *prefix, int http_status, const char *body) {
    for (int i = 0; i < d->count; i++) {
        if (!starts_with(d->q[i].post, prefix)) continue;
        const Pending p = d->q[i];
        memmove(&d->q[i], &d->q[i + 1], (size_t)(d->count - i - 1) * sizeof d->q[0]);
        d->count--;
        NkAchResponse r = {body, strlen(body), http_status};
        p.done(&r, p.done_ctx);
        return 1;
    }
    return 0;
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
    test_award_of_another_id_is_logged(ram);
    test_session_error_after_failed_load(ram);
    test_award_reply_after_unload(ram);
    free(ram);
    if (g_failures) {
        fprintf(stderr, "test_nk_achievements: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_achievements: PASS\n");
    return 0;
}
