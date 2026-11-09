/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Discord status (src/player/discord_presence.c, #2259): the frames and the exact JSON the
 * client sends, what it leaves out, the once-a-minute retry, the handshake, PING, CLOSE,
 * clearing, and on Windows the same exchange over a real named pipe served by this test.
 * Nothing here talks to Discord: the fake transport never leaves the process, and the
 * Windows pipe has a test-only name that is not one of Discord's (discord-ipc-<n>). The
 * application ID is a made-up value of the right shape. */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "discord_presence.h"
#include "nk_platform.h"
#include "native_test_isolation.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#define TEST_APP_ID "100000000000000001"
#define FAKE_BUF 65536

/* ---- a fake Discord on the other end of the transport ------------------------------- */

typedef struct {
    bool serve[NK_DISCORD_PIPE_COUNT]; /* which pipe numbers answer */
    char prefix[48];
    int opens;                         /* open calls, answered or not */
    char opened[64];                   /* the name that answered last */
    bool open;
    int closes;
    bool peer_closed;                  /* the next read reports the pipe closed */
    unsigned char out[FAKE_BUF];       /* what the client wrote */
    size_t out_used;
    size_t out_read;
    unsigned char in[FAKE_BUF];        /* what the fake Discord sends */
    size_t in_used;
    size_t in_read;
    size_t read_chunk;                 /* 0: all at once; else at most this many per read */
} FakeDiscord;

static bool fake_open(void *ctx, const char *name) {
    FakeDiscord *fake = (FakeDiscord *)ctx;
    fake->opens++;
    size_t n = strlen(fake->prefix);
    assert(strncmp(name, fake->prefix, n) == 0);
    int index = atoi(name + n);
    assert(index >= 0 && index < NK_DISCORD_PIPE_COUNT);
    if (!fake->serve[index]) return false;
    snprintf(fake->opened, sizeof(fake->opened), "%s", name);
    fake->open = true;
    fake->peer_closed = false;
    return true;
}

static bool fake_write(void *ctx, const void *data, size_t len) {
    FakeDiscord *fake = (FakeDiscord *)ctx;
    assert(fake->open);
    assert(fake->out_used + len <= sizeof(fake->out));
    memcpy(fake->out + fake->out_used, data, len);
    fake->out_used += len;
    return true;
}

static long fake_read(void *ctx, void *buf, size_t cap) {
    FakeDiscord *fake = (FakeDiscord *)ctx;
    assert(fake->open);
    if (fake->peer_closed) return -1;
    size_t avail = fake->in_used - fake->in_read;
    if (fake->read_chunk && avail > fake->read_chunk) avail = fake->read_chunk;
    if (avail > cap) avail = cap;
    memcpy(buf, fake->in + fake->in_read, avail);
    fake->in_read += avail;
    return (long)avail;
}

static void fake_close(void *ctx) {
    FakeDiscord *fake = (FakeDiscord *)ctx;
    assert(fake->open);
    fake->open = false;
    fake->closes++;
}

static void fake_init(FakeDiscord *fake, NkDiscordTransport *transport) {
    memset(fake, 0, sizeof(*fake));
    snprintf(fake->prefix, sizeof(fake->prefix), "fake-ipc-");
    transport->open = fake_open;
    transport->write = fake_write;
    transport->read = fake_read;
    transport->close = fake_close;
    transport->ctx = fake;
}

static void fake_send(FakeDiscord *fake, uint32_t opcode, const char *json) {
    size_t n = nk_discord_frame(fake->in + fake->in_used, sizeof(fake->in) - fake->in_used,
                                opcode, json, strlen(json));
    assert(n == strlen(json) + 8u);
    fake->in_used += n;
}

/* The next frame the client wrote, its payload NUL-terminated in json[cap]; false when the
   client wrote nothing more. */
static bool fake_next(FakeDiscord *fake, uint32_t *opcode, char *json, size_t cap) {
    if (fake->out_read == fake->out_used) return false;
    assert(fake->out_used - fake->out_read >= 8u);
    const unsigned char *h = fake->out + fake->out_read;
    *opcode = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) |
              ((uint32_t)h[3] << 24);
    uint32_t len = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) |
                   ((uint32_t)h[7] << 24);
    assert(fake->out_used - fake->out_read >= 8u + len && len < cap);
    memcpy(json, h + 8, len);
    json[len] = '\0';
    fake->out_read += 8u + len;
    return true;
}

static void expect_frame(FakeDiscord *fake, uint32_t opcode, const char *json) {
    uint32_t got_op = 99;
    char got[FAKE_BUF];
    assert(fake_next(fake, &got_op, got, sizeof(got)));
    if (got_op != opcode || strcmp(got, json) != 0) {
        fprintf(stderr, "FAIL: frame op %u %s\n  expected op %u %s\n", got_op, got, opcode,
                json);
        assert(0);
    }
}

static void expect_quiet(FakeDiscord *fake) {
    uint32_t op;
    char got[FAKE_BUF];
    if (fake_next(fake, &op, got, sizeof(got))) {
        fprintf(stderr, "FAIL: unexpected frame op %u %s\n", op, got);
        assert(0);
    }
}

/* The SET_ACTIVITY JSON this process sends for an activity (NULL clears). */
static void activity_json(char *out, size_t cap, const char *activity, unsigned nonce) {
    snprintf(out, cap,
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":%s},\"nonce\":\"%u\"}",
             nk_platform_process_id(), activity, nonce);
}

static const char k_ready[] =
    "{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1,\"config\":{\"cdn_host\":\"cdn.example\"},"
    "\"user\":{\"id\":\"200000000000000002\",\"username\":\"synthetic-user\"}},"
    "\"evt\":\"READY\",\"nonce\":null}";

/* ---- the pieces ---------------------------------------------------------------------- */

static void test_application_id(void) {
    assert(!nk_discord_application_id_valid(NULL));
    assert(!nk_discord_application_id_valid(""));
    assert(!nk_discord_application_id_valid("1234567890123456"));       /* 16 digits */
    assert(nk_discord_application_id_valid("12345678901234567"));        /* 17 */
    assert(nk_discord_application_id_valid("12345678901234567890"));     /* 20 */
    assert(!nk_discord_application_id_valid("123456789012345678901"));   /* 21 */
    assert(!nk_discord_application_id_valid("1234567890123456a"));
    assert(!nk_discord_application_id_valid(" 12345678901234567"));
    /* The placeholder fails closed: this build cannot turn the feature on until the
       maintainer sets the project's application ID. */
    assert(!nk_discord_application_id_valid(NK_DISCORD_APPLICATION_ID));
    assert(!nk_discord_available());
    /* Discord's own pipe names; the tests below never open them. */
    assert(strcmp(NK_DISCORD_PIPE_PREFIX, "discord-ipc-") == 0);
    assert(NK_DISCORD_PIPE_COUNT == 10);
    assert(NK_DISCORD_RETRY_MS == 60000u);
    printf("[DISCORD_PRESENCE_TEST] application id: PASS\n");
}

static void test_activity_make(void) {
    NkDiscordActivity a;
    assert(nk_discord_activity_make(&a, "  Hot Shots Tennis: Get a Grip ", "Ace",
                                    1762560000000));
    assert(strcmp(a.details, "Hot Shots Tennis: Get a Grip") == 0);
    assert(strcmp(a.state, "Ace") == 0);
    assert(a.start_ms == 1762560000000);

    assert(nk_discord_activity_make(&a, "Fate/Extra", "Authentic", 0));
    assert(strcmp(a.details, "Fate/Extra") == 0 && a.start_ms == 0);

    /* A backslash would be a Windows path: the title is left out, the mode stays. */
    assert(nk_discord_activity_make(&a, "C:\\path\\game.iso", "Enhanced", 5));
    assert(a.details[0] == '\0' && strcmp(a.state, "Enhanced") == 0);
    /* Too short for Discord, or not UTF-8: left out. */
    assert(nk_discord_activity_make(&a, "A", "Enhanced", 5) && a.details[0] == '\0');
    assert(nk_discord_activity_make(&a, "Bad \xC3\x28 bytes", "Enhanced", 5) &&
           a.details[0] == '\0');
    /* Discord's minimum counts characters: one 3-byte character is too short, two are not. */
    assert(nk_discord_activity_make(&a, "\xE3\x81\x82", "Enhanced", 5) && a.details[0] == '\0');
    assert(nk_discord_activity_make(&a, "\xE3\x81\x82\xE3\x81\x84", "Enhanced", 5) &&
           strcmp(a.details, "\xE3\x81\x82\xE3\x81\x84") == 0);
    /* A title cut short upstream inside a character keeps everything before it. */
    assert(nk_discord_activity_make(&a, "Test Game \xE3\x81", "Enhanced", 5) &&
           strcmp(a.details, "Test Game") == 0);
    /* Nothing left to show. */
    assert(!nk_discord_activity_make(&a, "", NULL, 5));
    assert(!nk_discord_activity_make(&a, NULL, "", 5));

    /* Cut to 128 characters on a character boundary: 130 two-byte characters. */
    char long_title[300];
    size_t used = 0;
    for (int i = 0; i < 130; i++) {
        long_title[used++] = '\xC3';
        long_title[used++] = '\xA9';
    }
    long_title[used] = '\0';
    assert(nk_discord_activity_make(&a, long_title, "Ace", 1));
    assert(strlen(a.details) == 256u && memcmp(a.details, long_title, 256u) == 0);
    printf("[DISCORD_PRESENCE_TEST] activity: PASS\n");
}

static void test_json_and_frames(void) {
    NkDiscordActivity a;
    char json[1024];
    assert(nk_discord_activity_make(&a, "Say \"Hi\" \x01 \xE3\x81\x82", "Enhanced",
                                    1762560000123));
    size_t n = nk_discord_activity_json(json, sizeof(json), 4242ul, &a, 7u);
    const char *want =
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":4242,\"activity\":{\"details\":"
        "\"Say \\\"Hi\\\" \\u0001 \xE3\x81\x82\",\"state\":\"Enhanced\",\"timestamps\":"
        "{\"start\":1762560000123}}},\"nonce\":\"7\"}";
    assert(n == strlen(want) && strcmp(json, want) == 0);

    /* Only what is there is sent: no title, no start. */
    assert(nk_discord_activity_make(&a, NULL, "Authentic", 0));
    n = nk_discord_activity_json(json, sizeof(json), 1ul, &a, 2u);
    assert(strcmp(json, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1,\"activity\":"
                        "{\"state\":\"Authentic\"}},\"nonce\":\"2\"}") == 0);
    /* Clearing. */
    n = nk_discord_activity_json(json, sizeof(json), 1ul, NULL, 3u);
    assert(strcmp(json, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1,\"activity\":null},"
                        "\"nonce\":\"3\"}") == 0);
    /* Too small: nothing, not a truncated command. */
    assert(nk_discord_activity_json(json, 20u, 1ul, NULL, 3u) == 0 && json[0] == '\0');

    unsigned char frame[32];
    assert(nk_discord_frame(frame, sizeof(frame), NK_DISCORD_OP_FRAME, "{}", 2) == 10u);
    static const unsigned char want_frame[] = {1, 0, 0, 0, 2, 0, 0, 0, '{', '}'};
    assert(memcmp(frame, want_frame, sizeof(want_frame)) == 0);
    assert(nk_discord_frame(frame, 9u, NK_DISCORD_OP_FRAME, "{}", 2) == 0);
    printf("[DISCORD_PRESENCE_TEST] json and frames: PASS\n");
}

/* ---- the client on the fake transport ---------------------------------------------------- */

static NkDiscordClient s_client;
static FakeDiscord s_fake;

static void test_unavailable_never_connects(void) {
    NkDiscordTransport t;
    fake_init(&s_fake, &t);
    s_fake.serve[0] = true;
    NkDiscordActivity a;
    assert(nk_discord_activity_make(&a, "Test Game", "Authentic", 1));
    /* The placeholder ID: nothing is opened, ever. */
    nk_discord_client_init(&s_client, NK_DISCORD_APPLICATION_ID, &t, s_fake.prefix);
    for (uint64_t now = 0; now < 5u * NK_DISCORD_RETRY_MS; now += 1000u) {
        nk_discord_client_update(&s_client, &a, now);
    }
    nk_discord_client_shutdown(&s_client);
    assert(s_fake.opens == 0 && s_fake.out_used == 0);
    printf("[DISCORD_PRESENCE_TEST] no application id, no connection: PASS\n");
}

static void test_retry_once_a_minute(void) {
    NkDiscordTransport t;
    fake_init(&s_fake, &t);
    nk_discord_client_init(&s_client, TEST_APP_ID, &t, s_fake.prefix);
    NkDiscordActivity a;
    assert(nk_discord_activity_make(&a, "Test Game", "Authentic", 1));
    /* Nothing to show: no attempt at all. */
    nk_discord_client_update(&s_client, NULL, 0);
    assert(s_fake.opens == 0);
    /* Discord not running: one attempt tries each pipe name once... */
    nk_discord_client_update(&s_client, &a, 1000u);
    assert(s_fake.opens == NK_DISCORD_PIPE_COUNT && s_client.attempts == 1u);
    /* ...and the next comes a minute later, however often the player asks. */
    for (uint64_t now = 1000u; now < 1000u + NK_DISCORD_RETRY_MS; now += 16u) {
        nk_discord_client_update(&s_client, &a, now);
    }
    assert(s_fake.opens == NK_DISCORD_PIPE_COUNT && s_client.attempts == 1u);
    nk_discord_client_update(&s_client, &a, 1000u + NK_DISCORD_RETRY_MS);
    assert(s_fake.opens == 2 * NK_DISCORD_PIPE_COUNT && s_client.attempts == 2u);
    assert(s_fake.out_used == 0 && s_client.link == NK_DISCORD_CLOSED);
    printf("[DISCORD_PRESENCE_TEST] retry once a minute: PASS\n");
}

static void test_session(void) {
    NkDiscordTransport t;
    char want[1024];
    fake_init(&s_fake, &t);
    s_fake.serve[2] = true;
    s_fake.read_chunk = 5; /* frames arrive in pieces */
    nk_discord_client_init(&s_client, TEST_APP_ID, &t, s_fake.prefix);
    NkDiscordActivity a;
    assert(nk_discord_activity_make(&a, "Test Game", "Enhanced", 1762560000000));

    nk_discord_client_update(&s_client, &a, 100u);
    assert(s_fake.opens == 3 && strcmp(s_fake.opened, "fake-ipc-2") == 0);
    expect_frame(&s_fake, NK_DISCORD_OP_HANDSHAKE, "{\"v\":1,\"client_id\":\"" TEST_APP_ID "\"}");
    /* Nothing more until Discord says READY. */
    nk_discord_client_update(&s_client, &a, 200u);
    expect_quiet(&s_fake);

    fake_send(&s_fake, NK_DISCORD_OP_FRAME, k_ready);
    for (int i = 0; i < 400 && s_client.link != NK_DISCORD_READY; i++) {
        nk_discord_client_update(&s_client, &a, 300u);
    }
    assert(s_client.link == NK_DISCORD_READY);
    activity_json(want, sizeof(want),
                  "{\"details\":\"Test Game\",\"state\":\"Enhanced\","
                  "\"timestamps\":{\"start\":1762560000000}}",
                  1u);
    expect_frame(&s_fake, NK_DISCORD_OP_FRAME, want);
    /* READY named the account; the client kept none of it. */
    {
        const unsigned char *bytes = (const unsigned char *)&s_client;
        const char *needle = "synthetic-user";
        size_t nlen = strlen(needle);
        for (size_t i = 0; i + nlen <= sizeof(s_client); i++) {
            assert(memcmp(bytes + i, needle, nlen) != 0);
        }
    }

    /* The same activity is not sent again; a changed one is. */
    nk_discord_client_update(&s_client, &a, 400u);
    expect_quiet(&s_fake);
    assert(nk_discord_activity_make(&a, "Test Game", "Ace", 1762560000000));
    nk_discord_client_update(&s_client, &a, 500u);
    activity_json(want, sizeof(want),
                  "{\"details\":\"Test Game\",\"state\":\"Ace\","
                  "\"timestamps\":{\"start\":1762560000000}}",
                  2u);
    expect_frame(&s_fake, NK_DISCORD_OP_FRAME, want);

    /* PING is answered with PONG and the same payload. */
    s_fake.read_chunk = 0;
    fake_send(&s_fake, NK_DISCORD_OP_PING, "{\"nonce\":\"p1\"}");
    nk_discord_client_update(&s_client, &a, 600u);
    expect_frame(&s_fake, NK_DISCORD_OP_PONG, "{\"nonce\":\"p1\"}");
    /* An error answer is noted, and the connection stays. */
    fake_send(&s_fake, NK_DISCORD_OP_FRAME,
              "{\"cmd\":\"SET_ACTIVITY\",\"evt\":\"ERROR\",\"data\":{\"code\":4000,"
              "\"message\":\"x\"},\"nonce\":\"2\"}");
    nk_discord_client_update(&s_client, &a, 700u);
    assert(s_client.last_error_code == 4000 && s_client.link == NK_DISCORD_READY);
    expect_quiet(&s_fake);

    /* The game exits: the status is cleared and the connection kept for the next game. */
    nk_discord_client_update(&s_client, NULL, 800u);
    activity_json(want, sizeof(want), "null", 3u);
    expect_frame(&s_fake, NK_DISCORD_OP_FRAME, want);
    assert(s_fake.open && s_fake.closes == 0);
    nk_discord_client_update(&s_client, NULL, 900u);
    expect_quiet(&s_fake);
    /* The next game shows at once, on the same connection. */
    assert(nk_discord_activity_make(&a, "Second Game", "Authentic", 1762560100000));
    nk_discord_client_update(&s_client, &a, 1000u);
    activity_json(want, sizeof(want),
                  "{\"details\":\"Second Game\",\"state\":\"Authentic\","
                  "\"timestamps\":{\"start\":1762560100000}}",
                  4u);
    expect_frame(&s_fake, NK_DISCORD_OP_FRAME, want);
    assert(s_fake.opens == 3);

    /* The player exits (or the setting goes off): clear, then close. */
    nk_discord_client_shutdown(&s_client);
    activity_json(want, sizeof(want), "null", 5u);
    expect_frame(&s_fake, NK_DISCORD_OP_FRAME, want);
    expect_quiet(&s_fake);
    assert(!s_fake.open && s_fake.closes == 1 && s_client.link == NK_DISCORD_CLOSED);
    printf("[DISCORD_PRESENCE_TEST] session: PASS\n");
}

static void test_dropped_connections(void) {
    NkDiscordTransport t;
    NkDiscordActivity a;
    assert(nk_discord_activity_make(&a, "Test Game", "Authentic", 1));

    /* No READY within the handshake timeout: closed, and the retry waits its minute. */
    fake_init(&s_fake, &t);
    s_fake.serve[0] = true;
    nk_discord_client_init(&s_client, TEST_APP_ID, &t, s_fake.prefix);
    nk_discord_client_update(&s_client, &a, 0u);
    assert(s_client.link == NK_DISCORD_WAITING_READY);
    nk_discord_client_update(&s_client, &a, NK_DISCORD_HANDSHAKE_TIMEOUT_MS - 1u);
    assert(s_client.link == NK_DISCORD_WAITING_READY);
    nk_discord_client_update(&s_client, &a, NK_DISCORD_HANDSHAKE_TIMEOUT_MS);
    assert(s_client.link == NK_DISCORD_CLOSED && s_fake.closes == 1);
    nk_discord_client_update(&s_client, &a, NK_DISCORD_RETRY_MS - 1u);
    assert(s_fake.opens == 1);
    nk_discord_client_update(&s_client, &a, NK_DISCORD_RETRY_MS);
    assert(s_fake.opens == 2 && s_client.link == NK_DISCORD_WAITING_READY);

    /* Discord sends CLOSE (a bad application ID does): closed quietly. */
    fake_send(&s_fake, NK_DISCORD_OP_CLOSE, "{\"code\":4000,\"message\":\"Invalid Client ID\"}");
    nk_discord_client_update(&s_client, &a, NK_DISCORD_RETRY_MS + 1u);
    assert(s_client.link == NK_DISCORD_CLOSED && s_fake.closes == 2);

    /* Discord quits mid-session: the read fails, the client closes and waits. */
    fake_init(&s_fake, &t);
    s_fake.serve[0] = true;
    nk_discord_client_init(&s_client, TEST_APP_ID, &t, s_fake.prefix);
    fake_send(&s_fake, NK_DISCORD_OP_FRAME, k_ready);
    nk_discord_client_update(&s_client, &a, 0u);
    assert(s_client.link == NK_DISCORD_READY && s_client.shown);
    s_fake.peer_closed = true;
    nk_discord_client_update(&s_client, &a, 10u);
    assert(s_client.link == NK_DISCORD_CLOSED && !s_client.shown && s_fake.closes == 1);
    nk_discord_client_update(&s_client, &a, 20u);
    assert(s_fake.opens == 1);

    /* A peer that floods PINGs gets at most NK_DISCORD_FRAMES_PER_UPDATE answers per
       update; the rest are answered on the following updates. */
    fake_init(&s_fake, &t);
    s_fake.serve[0] = true;
    nk_discord_client_init(&s_client, TEST_APP_ID, &t, s_fake.prefix);
    fake_send(&s_fake, NK_DISCORD_OP_FRAME, k_ready);
    nk_discord_client_update(&s_client, &a, 0u);
    assert(s_client.link == NK_DISCORD_READY);
    {
        uint32_t op;
        char got[256];
        while (fake_next(&s_fake, &op, got, sizeof(got))) {
        }
        for (int i = 0; i < 200; i++) fake_send(&s_fake, NK_DISCORD_OP_PING, "{}");
        int pongs = 0;
        nk_discord_client_update(&s_client, &a, 1u);
        while (fake_next(&s_fake, &op, got, sizeof(got))) {
            assert(op == NK_DISCORD_OP_PONG);
            pongs++;
        }
        assert(pongs == (int)NK_DISCORD_FRAMES_PER_UPDATE);
        for (uint64_t now = 2u; now < 10u; now++) {
            nk_discord_client_update(&s_client, &a, now);
            while (fake_next(&s_fake, &op, got, sizeof(got))) pongs++;
        }
        assert(pongs == 200 && s_client.link == NK_DISCORD_READY);
    }

    /* A frame longer than the client accepts: closed, not read past its buffer. */
    fake_init(&s_fake, &t);
    s_fake.serve[0] = true;
    nk_discord_client_init(&s_client, TEST_APP_ID, &t, s_fake.prefix);
    static const unsigned char huge[] = {1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F};
    memcpy(s_fake.in, huge, sizeof(huge));
    s_fake.in_used = sizeof(huge);
    nk_discord_client_update(&s_client, &a, 0u);
    assert(s_client.link == NK_DISCORD_CLOSED && s_fake.closes == 1);
    printf("[DISCORD_PRESENCE_TEST] dropped connections: PASS\n");
}

/* ---- Windows: a real named pipe served by this test ---------------------------------- */

#if defined(_WIN32) || defined(_WIN64)
static void server_read_exact(HANDLE pipe, void *buf, DWORD len) {
    DWORD got = 0;
    DWORD avail = 0;
    assert(PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) && avail >= len);
    assert(ReadFile(pipe, buf, len, &got, NULL) && got == len);
}

static void server_expect(HANDLE pipe, uint32_t opcode, const char *json) {
    unsigned char header[8];
    char payload[2048];
    server_read_exact(pipe, header, 8);
    uint32_t op = (uint32_t)header[0] | ((uint32_t)header[1] << 8);
    uint32_t len = (uint32_t)header[4] | ((uint32_t)header[5] << 8);
    assert(op == opcode && len < sizeof(payload));
    server_read_exact(pipe, payload, len);
    payload[len] = '\0';
    if (strcmp(payload, json) != 0) {
        fprintf(stderr, "FAIL: pipe frame %s\n  expected %s\n", payload, json);
        assert(0);
    }
}

static void test_windows_pipe(void) {
    char prefix[48];
    char name[64];
    char path[96];
    char want[1024];
    WCHAR wide[96];
    /* A name of this test's own: never one of Discord's pipes. */
    snprintf(prefix, sizeof(prefix), "nk-test-discord-%lu-", nk_platform_process_id());
    assert(strncmp(prefix, NK_DISCORD_PIPE_PREFIX, strlen(NK_DISCORD_PIPE_PREFIX)) != 0);
    snprintf(name, sizeof(name), "%s3", prefix);
    snprintf(path, sizeof(path), "\\\\.\\pipe\\%s", name);
    assert(MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 96) > 0);
    HANDLE server = CreateNamedPipeW(wide, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1,
                                     65536, 65536, 0, NULL);
    assert(server != INVALID_HANDLE_VALUE);

    assert(nk_platform_local_pipe_supported());
    NkLocalPipe probe;
    /* Names with a directory are refused before any open. */
    assert(!nk_platform_local_pipe_open(&probe, "..\\x"));
    snprintf(want, sizeof(want), "%s9", prefix);
    assert(!nk_platform_local_pipe_open(&probe, want)); /* nobody serves it */

    nk_discord_client_init(&s_client, TEST_APP_ID, NULL, prefix);
    NkDiscordActivity a;
    assert(nk_discord_activity_make(&a, "Pipe Game", "Ace", 1762560000000));
    nk_discord_client_update(&s_client, &a, 0u);
    assert(s_client.link == NK_DISCORD_WAITING_READY && s_client.pipe.open);
    BOOL connected = ConnectNamedPipe(server, NULL);
    assert(connected || GetLastError() == ERROR_PIPE_CONNECTED);
    server_expect(server, NK_DISCORD_OP_HANDSHAKE,
                  "{\"v\":1,\"client_id\":\"" TEST_APP_ID "\"}");
    /* Whoever serves the pipe may identify the player but never act as it: the client
       connects at identification level only. */
    {
        HANDLE token = NULL;
        SECURITY_IMPERSONATION_LEVEL level = SecurityImpersonation;
        DWORD size = 0;
        assert(ImpersonateNamedPipeClient(server));
        assert(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token));
        assert(GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof(level),
                                   &size));
        CloseHandle(token);
        assert(RevertToSelf());
        assert(level == SecurityIdentification);
    }

    unsigned char frame[1024];
    size_t n = nk_discord_frame(frame, sizeof(frame), NK_DISCORD_OP_FRAME, k_ready,
                                strlen(k_ready));
    DWORD wrote = 0;
    assert(WriteFile(server, frame, (DWORD)n, &wrote, NULL) && wrote == n);
    nk_discord_client_update(&s_client, &a, 10u);
    assert(s_client.link == NK_DISCORD_READY);
    activity_json(want, sizeof(want),
                  "{\"details\":\"Pipe Game\",\"state\":\"Ace\","
                  "\"timestamps\":{\"start\":1762560000000}}",
                  1u);
    server_expect(server, NK_DISCORD_OP_FRAME, want);

    /* The game exits. */
    nk_discord_client_update(&s_client, NULL, 20u);
    activity_json(want, sizeof(want), "null", 2u);
    server_expect(server, NK_DISCORD_OP_FRAME, want);

    /* The server goes away: the client notices on its next read and closes quietly. */
    DisconnectNamedPipe(server);
    CloseHandle(server);
    nk_discord_client_update(&s_client, &a, 30u);
    assert(s_client.link == NK_DISCORD_CLOSED && !s_client.pipe.open);
    nk_discord_client_shutdown(&s_client);
    printf("[DISCORD_PRESENCE_TEST] windows pipe: PASS\n");
}
#endif

int main(void) {
    native_test_create_root("discord-presence");
    native_test_isolate_user_data_roots();
    test_application_id();
    test_activity_make();
    test_json_and_frames();
    test_unavailable_never_connects();
    test_retry_once_a_minute();
    test_session();
    test_dropped_connections();
#if defined(_WIN32) || defined(_WIN64)
    test_windows_pipe();
#else
    assert(!nk_platform_local_pipe_supported());
    printf("[DISCORD_PRESENCE_TEST] windows pipe: SKIP (no local pipes on this host)\n");
#endif
    printf("[DISCORD_PRESENCE_TEST] all: PASS\n");
    return 0;
}
