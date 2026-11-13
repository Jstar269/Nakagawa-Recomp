// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The ad hoc relay's wire format (src/core/nk_adhoc_relay.c, #1139): every packet byte by byte,
 * the login checks of PPSSPP's ad hoc server (proAdhocServer.cpp login_user_data at 3bc60270:
 * product code, MAC, and a nickname refused only for a zero first byte),
 * the decoder's partial and refused input, and the port offset (PPSSPP's offset_port_simple).
 * The expected bytes are written out by hand from the packet layouts the relay protocol uses
 * (aemu_postoffice 02abf18f aemu_postoffice_packets.h and adhocctl/packets_v1.h; the lobby
 * layouts are also PPSSPP's Core/HLE/proAdhoc.h): little-endian, no padding. No socket and no
 * network. MACs, names and IDs are synthetic. */

#include "nk_adhoc_relay.h"

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

static const uint8_t k_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t k_peer[6] = {0x02, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5};

static int all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

static void test_sizes(void) {
    /* Each size is the sum of its packed fields. */
    CHECK(NK_LOBBY_LOGIN_SIZE == 1u + 6u + 128u + 9u, "login");
    CHECK(NK_LOBBY_CONNECT_SIZE == 1u + 8u, "connect");
    CHECK(NK_LOBBY_S2C_SCAN_SIZE == 1u + 8u + 6u, "scan");
    CHECK(NK_LOBBY_S2C_CONNECT_SIZE == 1u + 128u + 6u + 4u, "s2c connect");
    CHECK(NK_LOBBY_S2C_DISCONNECT_SIZE == 1u + 4u, "s2c disconnect");
    CHECK(NK_LOBBY_S2C_CONNECT_BSSID_SIZE == 1u + 6u, "bssid");
    CHECK(NK_LOBBY_S2C_CHAT_SIZE == 1u + 64u + 128u, "chat");
    CHECK(NK_RELAY_INIT_SIZE == 4u + 8u + 2u + 8u + 2u, "init");
    CHECK(NK_RELAY_PDP_HEADER_SIZE == 8u + 2u + 4u, "pdp header");
    CHECK(NK_RELAY_PTP_CONNECT_SIZE == 8u + 2u, "ptp connect");
}

static void test_login(void) {
    uint8_t p[NK_LOBBY_LOGIN_SIZE];
    memset(p, 0xee, sizeof p);
    CHECK(nk_lobby_login(p, k_mac, "Player", "ULUS10000") == 144u, "login size");
    CHECK(p[0] == 0x01, "opcode LOGIN");
    CHECK(memcmp(p + 1, k_mac, 6) == 0, "MAC at 1");
    CHECK(memcmp(p + 7, "Player", 6) == 0 && all_zero(p + 13, 128u - 6u), "nickname at 7, padded");
    CHECK(memcmp(p + 135, "ULUS10000", 9) == 0, "product code at 135");

    /* PPSSPP's server refuses a nickname only when its first byte is zero, so the longest is
     * the whole 128-byte field, sent with no terminator; 129 bytes do not fit the field. */
    char longest[129];
    memset(longest, 'n', 128);
    longest[128] = '\0';
    memset(p, 0xee, sizeof p);
    CHECK(nk_relay_nickname_ok(longest) && nk_lobby_login(p, k_mac, longest, "NPJH50000") == 144u &&
              p[7] == 'n' && p[7 + 127] == 'n' && memcmp(p + 135, "NPJH50000", 9) == 0,
          "a 128-byte nickname is accepted and fills the field with no terminator");
    char too_long[130];
    memset(too_long, 'n', 129);
    too_long[129] = '\0';
    memset(p, 0xee, sizeof p);
    CHECK(!nk_relay_nickname_ok(too_long) && nk_lobby_login(p, k_mac, too_long, "ULUS10000") == 0u &&
              p[0] == 0xee,
          "a 129-byte nickname, the first that does not fit, is refused and nothing is written");
    CHECK(nk_relay_nickname_ok("n") && nk_relay_nickname_ok("\x01"), "one non-zero byte is enough");
    CHECK(!nk_relay_nickname_ok(NULL) && nk_lobby_login(p, k_mac, NULL, "ULUS10000") == 0u,
          "NULL nickname");
    memset(p, 0xee, sizeof p);
    CHECK(!nk_relay_nickname_ok("") && nk_lobby_login(p, k_mac, "", "ULUS10000") == 0u && p[0] == 0xee,
          "an empty nickname is refused, as PPSSPP's server refuses one");

    /* PPSSPP's server refuses exactly the all-zero and the all-ones MAC. */
    const uint8_t zero[6] = {0};
    const uint8_t ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t zero_tail[6] = {0x02, 0, 0, 0, 0, 0};
    const uint8_t one_bit[6] = {0, 0, 0, 0, 0, 0x01};
    const uint8_t ones_but_one[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xfe};
    CHECK(!nk_relay_mac_ok(zero) && !nk_relay_mac_ok(ones) && !nk_relay_mac_ok(NULL),
          "the all-zero and all-ones MACs are refused");
    CHECK(nk_relay_mac_ok(zero_tail) && nk_relay_mac_ok(one_bit) && nk_relay_mac_ok(ones_but_one),
          "any other MAC is accepted");
    CHECK(nk_lobby_login(p, zero, "Player", "ULUS10000") == 0u &&
              nk_lobby_login(p, ones, "Player", "ULUS10000") == 0u,
          "login with a refused MAC");
    CHECK(nk_lobby_login(p, zero_tail, "Player", "ULUS10000") == 144u, "login with an accepted MAC");

    static const char *bad[] = {"ulus10000", "ULUS1000", "ULUS100000", "ULUS-0000", "", NULL};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(!nk_relay_product_ok(bad[i]) && nk_lobby_login(p, k_mac, "P", bad[i]) == 0u,
              "product code %s is refused", bad[i] ? bad[i] : "(null)");
    CHECK(nk_relay_product_ok("ULJM05000") && nk_relay_product_ok("NPUH10000"), "A-Z0-9 x9");
}

static void test_lobby_requests(void) {
    uint8_t p[NK_LOBBY_CONNECT_SIZE];
    const uint8_t group[8] = {'A', 'B', 'C', 'D', '1', '2', '3', '4'};
    CHECK(nk_lobby_connect(p, group) == 9u && p[0] == 0x02 && memcmp(p + 1, group, 8) == 0,
          "CONNECT is 02 + group");
    uint8_t b[1];
    CHECK(nk_lobby_bare(b, NK_LOBBY_PING) == 1u && b[0] == 0x00, "PING");
    CHECK(nk_lobby_bare(b, NK_LOBBY_SCAN) == 1u && b[0] == 0x04, "SCAN");
    CHECK(nk_lobby_bare(b, NK_LOBBY_DISCONNECT) == 1u && b[0] == 0x03, "DISCONNECT");
    CHECK(nk_lobby_bare(b, NK_LOBBY_LOGIN) == 0u, "LOGIN is not bare");
    CHECK(nk_lobby_bare(b, NK_LOBBY_CHAT) == 0u, "no chat is sent");
}

static void test_lobby_parse(void) {
    NkLobbyEvent ev;
    uint8_t scan[15] = {0x04, 'G', 'R', 'O', 'U', 'P', '0', '0', '1'};
    memcpy(scan + 9, k_peer, 6);
    CHECK(nk_lobby_parse(scan, 0, &ev) == NK_LOBBY_NEED_MORE, "empty");
    CHECK(nk_lobby_parse(scan, 14, &ev) == NK_LOBBY_NEED_MORE, "a SCAN one byte short");
    CHECK(nk_lobby_parse(scan, 15, &ev) == 15 && ev.kind == NK_LOBBY_SCAN &&
              memcmp(ev.group, "GROUP001", 8) == 0 && memcmp(ev.bssid, k_peer, 6) == 0,
          "SCAN: group and the group's BSSID");

    uint8_t connect[139];
    memset(connect, 0, sizeof connect);
    connect[0] = 0x02;
    memcpy(connect + 1, "Peer", 4);
    memcpy(connect + 129, k_peer, 6);
    connect[135] = 0x78;
    connect[136] = 0x56;
    connect[137] = 0x34;
    connect[138] = 0x12;
    CHECK(nk_lobby_parse(connect, 138, &ev) == NK_LOBBY_NEED_MORE, "CONNECT one byte short");
    CHECK(nk_lobby_parse(connect, 139, &ev) == 139 && ev.kind == NK_LOBBY_CONNECT &&
              strcmp(ev.nickname, "Peer") == 0 && memcmp(ev.mac, k_peer, 6) == 0 &&
              ev.peer_id == 0x12345678u,
          "CONNECT: nickname, MAC and little-endian peer id (%08x)", ev.peer_id);
    memset(connect + 1, 'x', 128);
    CHECK(nk_lobby_parse(connect, 139, &ev) == 139 && strlen(ev.nickname) == 128u &&
              ev.nickname[127] == 'x' && memcmp(ev.mac, k_peer, 6) == 0,
          "an unterminated 128-byte nickname from the server is kept whole and terminated");

    const uint8_t disconnect[5] = {0x03, 0x01, 0x00, 0x00, 0x80};
    CHECK(nk_lobby_parse(disconnect, 5, &ev) == 5 && ev.kind == NK_LOBBY_DISCONNECT &&
              ev.peer_id == 0x80000001u,
          "DISCONNECT: peer id");
    uint8_t bssid[7] = {0x06};
    memcpy(bssid + 1, k_peer, 6);
    CHECK(nk_lobby_parse(bssid, 7, &ev) == 7 && ev.kind == NK_LOBBY_CONNECT_BSSID &&
              memcmp(ev.bssid, k_peer, 6) == 0,
          "CONNECT_BSSID: the group's BSSID");
    const uint8_t done[1] = {0x05};
    CHECK(nk_lobby_parse(done, 1, &ev) == 1 && ev.kind == NK_LOBBY_SCAN_COMPLETE, "SCAN_COMPLETE");
    uint8_t chat[193];
    memset(chat, 0, sizeof chat);
    chat[0] = 0x07;
    memcpy(chat + 1, "hello", 5);
    memcpy(chat + 65, "Peer", 4);
    CHECK(nk_lobby_parse(chat, 192, &ev) == NK_LOBBY_NEED_MORE, "CHAT one byte short");
    CHECK(nk_lobby_parse(chat, 193, &ev) == 193 && ev.kind == NK_LOBBY_CHAT &&
              strcmp(ev.nickname, "Peer") == 0,
          "CHAT is used up whole");

    const uint8_t never[3] = {0x00, 0x01, 0x08};
    for (int i = 0; i < 3; i++)
        CHECK(nk_lobby_parse(&never[i], 1, &ev) == NK_LOBBY_BAD,
              "opcode %u never comes from a server", never[i]);

    /* Two packets in one read: the first is decoded and only its bytes are used. */
    uint8_t two[16];
    memcpy(two, scan, 15);
    two[15] = 0x05;
    CHECK(nk_lobby_parse(two, 16, &ev) == 15 && nk_lobby_parse(two + 15, 1, &ev) == 1 &&
              ev.kind == NK_LOBBY_SCAN_COMPLETE,
          "back-to-back packets");
}

static void test_relay_init(void) {
    uint8_t p[NK_RELAY_INIT_SIZE];
    memset(p, 0xee, sizeof p);
    CHECK(nk_relay_init(p, NK_RELAY_INIT_PDP, k_mac, 0x2b67u, NULL, 0) == 24u, "PDP init");
    const uint8_t pdp[24] = {0, 0, 0, 0, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x67, 0x2b,
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(memcmp(p, pdp, 24) == 0, "PDP init bytes");
    CHECK(nk_relay_init(p, NK_RELAY_INIT_PTP_LISTEN, k_mac, 0x2b67u, k_peer, 7) == 24u &&
              p[0] == 1 && all_zero(p + 14, 10),
          "a listen names only the local end");
    CHECK(nk_relay_init(p, NK_RELAY_INIT_PTP_CONNECT, k_mac, 0x1001u, k_peer, 0x2002u) == 24u,
          "PTP connect init");
    const uint8_t conn[24] = {2, 0, 0, 0, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x01, 0x10,
                              0x02, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0, 0, 0x02, 0x20};
    CHECK(memcmp(p, conn, 24) == 0, "PTP connect init bytes");
    CHECK(nk_relay_init(p, NK_RELAY_INIT_PTP_ACCEPT, k_mac, 1, k_peer, 2) == 24u && p[0] == 3,
          "PTP accept init");
    memset(p, 0xee, sizeof p);
    CHECK(nk_relay_init(p, NK_RELAY_INIT_PTP_ACCEPT, k_mac, 1, NULL, 2) == 0u && p[0] == 0xee,
          "an accept needs the peer");
    CHECK(nk_relay_init(p, NK_RELAY_INIT_PTP_CONNECT, k_mac, 1, NULL, 2) == 0u, "connect too");
    CHECK(nk_relay_init(p, (NkRelayInit)4, k_mac, 1, NULL, 0) == 0u, "no unreliable UDP form");
    CHECK(nk_relay_init(p, NK_RELAY_INIT_PDP, NULL, 1, NULL, 0) == 0u, "NULL source");
}

static void test_relay_data(void) {
    uint8_t h[NK_RELAY_PDP_HEADER_SIZE];
    CHECK(nk_relay_pdp_header(h, k_peer, 0x1234u, NK_RELAY_PDP_MAX) == 14u, "PDP header");
    const uint8_t want[14] = {0x02, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0, 0, 0x34, 0x12,
                              0x00, 0x28, 0x00, 0x00};
    CHECK(memcmp(h, want, 14) == 0, "PDP header bytes (10240 = 0x2800)");
    CHECK(nk_relay_pdp_header(h, k_peer, 1, NK_RELAY_PDP_MAX + 1u) == 0u, "a datagram over 10 KiB");
    CHECK(nk_relay_pdp_header(h, k_peer, 1, 0u) == 14u, "an empty datagram is allowed");
    uint8_t mac[6];
    uint16_t port = 0;
    uint32_t size = 0;
    CHECK(nk_relay_parse_pdp_header(want, mac, &port, &size) && memcmp(mac, k_peer, 6) == 0 &&
              port == 0x1234u && size == 10240u,
          "PDP header round trip");
    uint8_t big[14];
    memcpy(big, want, 14);
    big[10] = 0x01;  /* 10241 */
    CHECK(!nk_relay_parse_pdp_header(big, mac, &port, &size), "a sender over 10 KiB ends the session");

    const uint8_t pc[10] = {0x02, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0x77, 0x77, 0xb8, 0x22};
    CHECK(nk_relay_parse_ptp_connect(pc, mac, &port) && memcmp(mac, k_peer, 6) == 0 &&
              port == 0x22b8u,
          "PTP connect request: the connector's MAC and port");

    uint8_t d[4];
    CHECK(nk_relay_ptp_data_header(d, NK_RELAY_PTP_MAX) == 4u && d[0] == 0x00 && d[1] == 0xc8 &&
              d[2] == 0 && d[3] == 0,
          "PTP block size 51200 = 0xc800");
    CHECK(nk_relay_ptp_data_header(d, NK_RELAY_PTP_MAX + 1u) == 0u, "a block over 50 KiB");
    const uint8_t over[4] = {0x01, 0xc8, 0x00, 0x00};
    CHECK(!nk_relay_parse_ptp_data_header(over, &size), "a received block over 50 KiB");
    CHECK(nk_relay_parse_ptp_data_header(d, &size) && size == 51200u, "PTP size round trip");
}

static void test_ports(void) {
    CHECK(nk_relay_port_out(0, NK_RELAY_PORT_OFFSET) == 0u, "port 0 stays 0");
    CHECK(nk_relay_port_out(1000, NK_RELAY_PORT_OFFSET) == 11000u, "1000 -> 11000");
    CHECK(nk_relay_port_out(55536, NK_RELAY_PORT_OFFSET) == 65535u, "a port that wraps to 0");
    CHECK(nk_relay_port_out(60000, NK_RELAY_PORT_OFFSET) == 4464u, "a port that wraps past 0");
    CHECK(nk_relay_port_in(11000, NK_RELAY_PORT_OFFSET) == 1000u, "11000 -> 1000");
    CHECK(nk_relay_port_in(4464, NK_RELAY_PORT_OFFSET) == 60000u, "and back across the wrap");
    CHECK(nk_relay_port_in(0, NK_RELAY_PORT_OFFSET) == 0u, "port 0 comes back as 0, as it went out");
    CHECK(nk_relay_port_in(65535, NK_RELAY_PORT_OFFSET) == 55535u,
          "65535 comes back as 55535: 55536, which went out as 65535, does not round trip, as in PPSSPP");
    CHECK(NK_RELAY_LOBBY_PORT == 27312u && NK_RELAY_DATA_PORT == 27313u, "the two ports");
}

/* Hostile and short input (review of #2549 at eaebd9c1b5e9, tests 3 to 5). */
static void test_short_reads(void) {
    static const struct { uint8_t op; int need; } k_s2c[] = {
        {0x02, 139}, {0x03, 5}, {0x04, 15}, {0x05, 1}, {0x06, 7}, {0x07, 193}};
    uint8_t buf[200];
    NkLobbyEvent ev;
    for (size_t k = 0; k < sizeof k_s2c / sizeof k_s2c[0]; k++) {
        memset(buf, 0x5a, sizeof buf);
        buf[0] = k_s2c[k].op;
        for (int n = 1; n < k_s2c[k].need; n++)
            CHECK(nk_lobby_parse(buf, (size_t)n, &ev) == NK_LOBBY_NEED_MORE,
                  "opcode %u at %d of %d bytes is a short read", (unsigned)k_s2c[k].op, n, k_s2c[k].need);
        CHECK(nk_lobby_parse(buf, (size_t)k_s2c[k].need, &ev) == k_s2c[k].need,
              "opcode %u whole at %d bytes", (unsigned)k_s2c[k].op, k_s2c[k].need);
    }
    const uint8_t disconnect4[4] = {0x03, 0x01, 0x00, 0x00};
    CHECK(nk_lobby_parse(disconnect4, 4, &ev) == NK_LOBBY_NEED_MORE, "DISCONNECT one byte short");
    const uint8_t bssid6[6] = {0x06, 0x02, 0xa1, 0xb2, 0xc3, 0xd4};
    CHECK(nk_lobby_parse(bssid6, 6, &ev) == NK_LOBBY_NEED_MORE, "CONNECT_BSSID one byte short");
    for (unsigned op = 0; op < 256u; op++) {
        if (op >= 0x02u && op <= 0x07u) continue;
        const uint8_t one[1] = {(uint8_t)op};
        CHECK(nk_lobby_parse(one, 1, &ev) == NK_LOBBY_BAD, "opcode %u from a server is refused", op);
    }
}

static void test_size_fields_are_32_bits(void) {
    static const uint8_t k_sizes[3][4] = {{0x00, 0x00, 0x01, 0x00}, {0x00, 0x00, 0x00, 0x80},
                                          {0xff, 0xff, 0xff, 0xff}};
    for (size_t k = 0; k < 3; k++) {
        uint8_t h[NK_RELAY_PDP_HEADER_SIZE] = {0x02, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0, 0, 0x34, 0x12};
        memcpy(h + 10, k_sizes[k], 4);
        uint8_t mac[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
        uint16_t port = 0x7777u;
        uint32_t size = 0x77777777u;
        CHECK(!nk_relay_parse_pdp_header(h, mac, &port, &size) && mac[0] == 0x77 && port == 0x7777u &&
                  size == 0x77777777u,
              "PDP size %02x %02x %02x %02x is refused with the outputs untouched", k_sizes[k][0],
              k_sizes[k][1], k_sizes[k][2], k_sizes[k][3]);
    }
    uint32_t size = 0x77777777u;
    CHECK(!nk_relay_parse_ptp_data_header(k_sizes[0], &size) && size == 0x77777777u,
          "PTP size 00 00 01 00 (65536) is refused with the output untouched");
}

/* A CONNECT carrying values no real console holds. The codec checks only opcode and length, so
 * each is decoded as sent; the consumer validates (nk_adhoc_relay.c, known limits). */
static void test_hostile_connect(void) {
    static const uint8_t k_macs[4][6] = {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
                                         {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01},
                                         {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                                         {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}}; /* k_mac, the console's own */
    uint8_t c[139];
    NkLobbyEvent ev;
    for (size_t k = 0; k < 4; k++) {
        memset(c, 0, sizeof c);
        c[0] = 0x02;
        memcpy(c + 1, "Peer", 4);
        memcpy(c + 129, k_macs[k], 6);
        c[135] = 0x02;
        CHECK(nk_lobby_parse(c, 139, &ev) == 139 && memcmp(ev.mac, k_macs[k], 6) == 0 && ev.peer_id == 2u,
              "a CONNECT with MAC %02x:%02x:.. decodes as sent", k_macs[k][0], k_macs[k][1]);
    }
    memset(c, 0, sizeof c);
    c[0] = 0x02;
    memcpy(c + 129, k_peer, 6);
    CHECK(nk_lobby_parse(c, 139, &ev) == 139 && ev.nickname[0] == '\0',
          "a CONNECT with an empty nickname decodes as sent");
}

static void test_check_boundaries(void) {
    CHECK(nk_relay_product_ok("AZ0912345") && nk_relay_product_ok("ULUS10000"), "A-Z and 0-9");
    CHECK(!nk_relay_product_ok("@LUS10000") && !nk_relay_product_ok("[LUS10000") &&
              !nk_relay_product_ok("ULUS1000/") && !nk_relay_product_ok("ULUS1000:"),
          "the bytes either side of A-Z and 0-9 are refused");
    CHECK(!nk_relay_product_ok("ULUS1000") && !nk_relay_product_ok("ULUS100000") &&
              !nk_relay_product_ok(""),
          "eight, ten and no characters are refused");
    uint8_t *field = (uint8_t *)malloc(NK_RELAY_NICKNAME_LEN); /* exactly the field: no terminator */
    CHECK(field != NULL, "allocate a field");
    if (field) {
        memset(field, 'n', NK_RELAY_NICKNAME_LEN);
        CHECK(nk_relay_nickname_field_ok(field), "a full 128-byte field with no terminator");
        field[0] = 0u;
        CHECK(!nk_relay_nickname_field_ok(field), "a field whose first byte is zero");
        free(field);
    }
    CHECK(!nk_relay_nickname_field_ok(NULL), "no field");
    /* The field at the end of a buffer, then a guard byte the check must never read: its answer
     * is the same whatever the guard holds. */
    uint8_t guarded[NK_RELAY_NICKNAME_LEN + 1u];
    memset(guarded, 'n', NK_RELAY_NICKNAME_LEN);
    for (unsigned guard = 0; guard < 256u; guard++) {
        guarded[NK_RELAY_NICKNAME_LEN] = (uint8_t)guard;
        CHECK(nk_relay_nickname_field_ok(guarded), "a full field followed by guard byte %u", guard);
    }
}

int main(void) {
    test_sizes();
    test_login();
    test_lobby_requests();
    test_lobby_parse();
    test_relay_init();
    test_relay_data();
    test_ports();
    test_short_reads();
    test_size_fields_are_32_bits();
    test_hostile_connect();
    test_check_boundaries();
    if (g_failures) {
        fprintf(stderr, "test_nk_adhoc_relay: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_adhoc_relay: PASS\n");
    return 0;
}
