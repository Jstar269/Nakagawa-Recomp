// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The ad hoc transport seam and its in-process medium (src/core/nk_adhoc_net.c, #1139 phase 2):
 * a console alone, and two to four simulated consoles in one process. The lobby rules checked here
 * are those of PPSSPP's ad hoc server (Core/HLE/proAdhocServer.cpp at 3bc60270, which the medium's
 * lobby is translated from): who hears of a join and a leave and in what order (newest member
 * first), the group's own BSSID, scans per product code with the newest group first, and the
 * requests the server answers by logging the client out. The MACs are chosen so that MAC order,
 * join order and newest-first order all differ. No socket and no network. MACs, names and product
 * codes are synthetic. */

#include "nk_adhoc_net.h"

#include <stdio.h>
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

/* Ascending MAC order is B, C, A, D: neither join order (A, B, C, D) nor newest first. */
static const uint8_t k_a[6] = {0x02, 0x50, 0x00, 0x00, 0x00, 0x0a};
static const uint8_t k_b[6] = {0x02, 0x10, 0x00, 0x00, 0x00, 0x0b};
static const uint8_t k_c[6] = {0x02, 0x30, 0x00, 0x00, 0x00, 0x0c};
static const uint8_t k_d[6] = {0x02, 0x60, 0x00, 0x00, 0x00, 0x0d};
static const uint8_t k_g1[8] = {'G', 'R', 'O', 'U', 'P', '0', '0', '1'};
static const uint8_t k_g2[8] = {'A', 'L', 'P', 'H', 'A', '0', '0', '2'};
static const uint8_t k_g3[8] = {'M', 'I', 'K', 'E', '0', '0', '0', '3'};
static const char k_game[] = "ULUS10000";
static const char k_other_game[] = "ULES20000";

enum { A = 0, B = 1, C = 2, D = 3 };
/* peer ids: console index plus one */
enum { ID_A = 1, ID_B = 2, ID_C = 3, ID_D = 4 };

static int login(NkAdhocTransport *t, const uint8_t *mac, const char *nick, const char *product) {
    return t->ops->login(t, mac, nick, product);
}

static size_t take(NkAdhocTransport *t, NkLobbyEvent *ev, size_t max) {
    return t->ops->take_events(t, ev, max);
}

static int is_own_bssid(const uint8_t *mac) { return (mac[0] & 0x03u) == 0x02u; }

/* The medium's trace since the last call must be exactly `want`, in order. */
static int trace_is(NkAdhocLoopNet *net, const NkLoopTrace *want, size_t n, const char *what) {
    NkLoopTrace got[NK_LOOPNET_TRACE];
    const size_t queued = nk_loopnet_trace(net, got, NK_LOOPNET_TRACE);
    int ok = queued == n;
    for (size_t i = 0; ok && i < n; i++)
        ok = got[i].console == want[i].console && got[i].kind == want[i].kind &&
             (want[i].kind != NK_LOBBY_CONNECT && want[i].kind != NK_LOBBY_DISCONNECT ?
                  1 : got[i].peer_id == want[i].peer_id);
    if (!ok) {
        fprintf(stderr, "  %s: got %zu events:", what, queued);
        for (size_t i = 0; i < queued && i < NK_LOOPNET_TRACE; i++)
            fprintf(stderr, " (%u,%d,%u)", got[i].console, (int)got[i].kind, (unsigned)got[i].peer_id);
        fputc('\n', stderr);
    }
    return ok;
}

static void test_alone(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0);
    NkLobbyEvent ev[8];
    CHECK(strcmp(a->ops->name, "loopback") == 0, "transport name");
    CHECK(nk_loopnet_console(net, NK_LOOPNET_CONSOLES) == NULL, "no console past the last");
    CHECK(a->ops->connect(a, k_g1) == NK_NET_NOT_LOGGED_IN, "connect before login is refused");
    CHECK(a->ops->scan(a) == NK_NET_NOT_LOGGED_IN, "scan before login is refused");
    CHECK(a->ops->disconnect(a) == NK_NET_NOT_LOGGED_IN, "leave before login is refused");
    CHECK(a->ops->pdp_open(a, 0x1234) == NK_NET_NOT_LOGGED_IN, "a socket before login is refused");
    CHECK(login(a, k_a, "Alone", k_game) == NK_NET_OK, "login");
    CHECK(login(a, k_a, "Alone", k_game) == NK_NET_BAD_ARG, "a second login is refused");

    CHECK(a->ops->scan(a) == NK_NET_OK, "scan");
    CHECK(take(a, ev, 8) == 1 && ev[0].kind == NK_LOBBY_SCAN_COMPLETE, "an empty scan only completes");

    CHECK(a->ops->connect(a, k_g1) == NK_NET_OK, "create");
    size_t n = take(a, ev, 8);
    CHECK(n == 1 && ev[0].kind == NK_LOBBY_CONNECT_BSSID && is_own_bssid(ev[0].mac) &&
              memcmp(ev[0].mac, k_a, 6) != 0,
          "a creator alone hears only the group's BSSID, a locally administered address that is "
          "not its MAC (got %zu events)", n);

    int s = a->ops->pdp_open(a, 0x1234);
    CHECK(s >= 0, "open a socket");
    CHECK(a->ops->pdp_open(a, 0x1234) == NK_NET_PORT_IN_USE, "the same port twice is refused");
    CHECK(a->ops->pdp_open(a, 0) == NK_NET_BAD_ARG, "port 0 is the caller's to assign");
    CHECK(a->ops->pdp_send(a, s, k_a, 0x1234, "ping", 4) == NK_NET_OK, "send to self");
    uint8_t mac[6], buf[16];
    uint16_t port = 0;
    uint32_t len = 0;
    CHECK(a->ops->pdp_recv(a, s, mac, &port, buf, 2, &len) == NK_NET_SHORT_BUFFER && len == 4,
          "a short buffer is told the size");
    CHECK(a->ops->pdp_recv(a, s, mac, &port, buf, sizeof buf, &len) == NK_NET_OK && len == 4 &&
              memcmp(buf, "ping", 4) == 0 && memcmp(mac, k_a, 6) == 0 && port == 0x1234,
          "the datagram stayed queued and arrives whole from self");
    CHECK(a->ops->pdp_recv(a, s, mac, &port, buf, sizeof buf, &len) == NK_NET_AGAIN, "then nothing");
    nk_loopnet_destroy(net);
}

static void test_login_rules(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0), *b = nk_loopnet_console(net, 1);
    static const uint8_t zero[6] = {0};
    static const uint8_t ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    CHECK(login(a, zero, "Alpha", k_game) == NK_NET_BAD_ARG, "the all-zero MAC is refused");
    CHECK(login(a, ones, "Alpha", k_game) == NK_NET_BAD_ARG, "the all-ones MAC is refused");
    CHECK(login(a, k_a, "", k_game) == NK_NET_BAD_ARG, "an empty nickname is refused");
    CHECK(login(a, k_a, "Alpha", "ulus10000") == NK_NET_BAD_ARG, "a product code in lower case");
    CHECK(login(a, k_a, "Alpha", NULL) == NK_NET_BAD_ARG && login(a, NULL, "Alpha", k_game) == NK_NET_BAD_ARG,
          "NULL arguments");
    CHECK(a->ops->scan(a) == NK_NET_NOT_LOGGED_IN, "a refused login leaves the console logged out");
    CHECK(login(a, k_a, "Alpha", k_game) == NK_NET_OK, "then a valid login");
    CHECK(login(b, k_a, "Copy", k_game) == NK_NET_BAD_ARG,
          "a MAC already logged in is refused (the medium's rule; PPSSPP's server accepts it)");
    nk_loopnet_destroy(net);
}

/* connect_user: each member, newest first, hears of the joiner, then the joiner of that member;
 * the joiner then gets the group's BSSID. */
static void test_join_order(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, A), *b = nk_loopnet_console(net, B),
                     *c = nk_loopnet_console(net, C), *d = nk_loopnet_console(net, D);
    NkLobbyEvent ev[8];
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    login(c, k_c, "Charlie", k_game);
    login(d, k_d, "Delta", k_game);
    nk_loopnet_trace(net, NULL, 0);

    CHECK(a->ops->connect(a, k_g1) == NK_NET_OK, "A creates the group");
    static const NkLoopTrace t_a[] = {{A, NK_LOBBY_CONNECT_BSSID, 0}};
    CHECK(trace_is(net, t_a, 1, "A creates"), "the creator alone hears the BSSID");
    CHECK(take(a, ev, 8) == 1, "one event for A");
    uint8_t bssid[6];
    memcpy(bssid, ev[0].mac, 6);

    CHECK(b->ops->connect(b, k_g1) == NK_NET_OK, "B joins");
    static const NkLoopTrace t_b[] = {
        {A, NK_LOBBY_CONNECT, ID_B}, {B, NK_LOBBY_CONNECT, ID_A}, {B, NK_LOBBY_CONNECT_BSSID, 0}};
    CHECK(trace_is(net, t_b, 3, "B joins"), "A hears of B, then B of A, then B gets the BSSID");
    size_t n = take(b, ev, 8);
    CHECK(n == 2 && ev[0].kind == NK_LOBBY_CONNECT && memcmp(ev[0].mac, k_a, 6) == 0 &&
              strcmp(ev[0].nickname, "Alpha") == 0 && ev[0].peer_id == ID_A &&
              ev[1].kind == NK_LOBBY_CONNECT_BSSID && memcmp(ev[1].mac, bssid, 6) == 0,
          "B hears of A with A's MAC, nickname and id, then the group's BSSID");
    n = take(a, ev, 8);
    CHECK(n == 1 && ev[0].kind == NK_LOBBY_CONNECT && memcmp(ev[0].mac, k_b, 6) == 0 &&
              strcmp(ev[0].nickname, "Bravo") == 0 && ev[0].peer_id == ID_B,
          "A hears of B");

    c->ops->connect(c, k_g1);
    static const NkLoopTrace t_c[] = {{B, NK_LOBBY_CONNECT, ID_C}, {C, NK_LOBBY_CONNECT, ID_B},
                                      {A, NK_LOBBY_CONNECT, ID_C}, {C, NK_LOBBY_CONNECT, ID_A},
                                      {C, NK_LOBBY_CONNECT_BSSID, 0}};
    CHECK(trace_is(net, t_c, 5, "C joins"), "C joins: B (newest) first, then A");

    /* D joins A, B and C: newest first is C, B, A; join order would be A, B, C and MAC order B, C, A. */
    d->ops->connect(d, k_g1);
    static const NkLoopTrace t_d[] = {{C, NK_LOBBY_CONNECT, ID_D}, {D, NK_LOBBY_CONNECT, ID_C},
                                      {B, NK_LOBBY_CONNECT, ID_D}, {D, NK_LOBBY_CONNECT, ID_B},
                                      {A, NK_LOBBY_CONNECT, ID_D}, {D, NK_LOBBY_CONNECT, ID_A},
                                      {D, NK_LOBBY_CONNECT_BSSID, 0}};
    CHECK(trace_is(net, t_d, 7, "D joins"), "D joins: members told newest first, each in turn with D");
    n = take(d, ev, 8);
    CHECK(n == 4 && memcmp(ev[0].mac, k_c, 6) == 0 && memcmp(ev[1].mac, k_b, 6) == 0 &&
              memcmp(ev[2].mac, k_a, 6) == 0 && ev[3].kind == NK_LOBBY_CONNECT_BSSID &&
              memcmp(ev[3].mac, bssid, 6) == 0,
          "D hears of C, B, A, then the same BSSID");
    take(a, ev, 8);
    take(b, ev, 8);
    take(c, ev, 8);
    nk_loopnet_destroy(net);
}

/* disconnect_user: the remaining members, newest first, hear the leaver's id; the leaver hears
 * nothing; the BSSID outlives the creator; an empty group goes, and one made again is new. */
static void test_leave_order(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, A), *b = nk_loopnet_console(net, B),
                     *c = nk_loopnet_console(net, C), *d = nk_loopnet_console(net, D);
    NkLobbyEvent ev[8];
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    login(c, k_c, "Charlie", k_game);
    login(d, k_d, "Delta", k_game);
    a->ops->connect(a, k_g1);
    take(a, ev, 8);
    uint8_t bssid[6];
    memcpy(bssid, ev[0].mac, 6);
    b->ops->connect(b, k_g1);
    c->ops->connect(c, k_g1);
    d->ops->connect(d, k_g1);
    take(a, ev, 8);
    take(b, ev, 8);
    take(c, ev, 8);
    take(d, ev, 8);
    nk_loopnet_trace(net, NULL, 0);

    /* Members newest first: D, C, B, A. B leaves from the middle. */
    CHECK(b->ops->disconnect(b) == NK_NET_OK, "B leaves");
    static const NkLoopTrace t_b[] = {
        {D, NK_LOBBY_DISCONNECT, ID_B}, {C, NK_LOBBY_DISCONNECT, ID_B}, {A, NK_LOBBY_DISCONNECT, ID_B}};
    CHECK(trace_is(net, t_b, 3, "B leaves"), "D, C, then A hear B leave");
    CHECK(take(b, ev, 8) == 0, "the leaver hears nothing");
    CHECK(take(a, ev, 8) == 1 && ev[0].kind == NK_LOBBY_DISCONNECT && ev[0].peer_id == ID_B,
          "A hears B's id");

    /* The creator leaves; the group keeps its BSSID. */
    a->ops->disconnect(a);
    static const NkLoopTrace t_a[] = {{D, NK_LOBBY_DISCONNECT, ID_A}, {C, NK_LOBBY_DISCONNECT, ID_A}};
    CHECK(trace_is(net, t_a, 2, "A leaves"), "D, then C hear A leave");
    a->ops->scan(a);
    size_t n = take(a, ev, 8);
    CHECK(n == 2 && ev[0].kind == NK_LOBBY_SCAN && memcmp(ev[0].group, k_g1, 8) == 0 &&
              memcmp(ev[0].mac, bssid, 6) == 0 && ev[1].kind == NK_LOBBY_SCAN_COMPLETE,
          "the group outlives its creator with the same BSSID");

    /* B joins again: it hears of D and C, newest first, and the unchanged BSSID. */
    nk_loopnet_trace(net, NULL, 0);
    b->ops->connect(b, k_g1);
    static const NkLoopTrace t_rejoin[] = {{D, NK_LOBBY_CONNECT, ID_B}, {B, NK_LOBBY_CONNECT, ID_D},
                                           {C, NK_LOBBY_CONNECT, ID_B}, {B, NK_LOBBY_CONNECT, ID_C},
                                           {B, NK_LOBBY_CONNECT_BSSID, 0}};
    CHECK(trace_is(net, t_rejoin, 5, "B rejoins"), "a rejoin is a join");
    n = take(b, ev, 8);
    CHECK(n == 3 && memcmp(ev[2].mac, bssid, 6) == 0, "the BSSID did not change");

    /* Now B is the newest member: when D leaves, B hears first. */
    d->ops->disconnect(d);
    static const NkLoopTrace t_d[] = {{B, NK_LOBBY_DISCONNECT, ID_D}, {C, NK_LOBBY_DISCONNECT, ID_D}};
    CHECK(trace_is(net, t_d, 2, "D leaves"), "B (newest), then C hear D leave");

    /* Logging out leaves the group the same way. */
    c->ops->logout(c);
    static const NkLoopTrace t_c[] = {{B, NK_LOBBY_DISCONNECT, ID_C}};
    CHECK(trace_is(net, t_c, 1, "C logs out"), "B hears C log out");
    CHECK(c->ops->disconnect(c) == NK_NET_NOT_LOGGED_IN, "after logout the lobby is closed");

    /* The last member leaves: the group goes, and one made again has a new BSSID. */
    b->ops->disconnect(b);
    CHECK(trace_is(net, NULL, 0, "the last leaves"), "nobody is left to tell");
    take(a, ev, 8);
    take(b, ev, 8);
    a->ops->scan(a);
    CHECK(take(a, ev, 8) == 1 && ev[0].kind == NK_LOBBY_SCAN_COMPLETE, "the empty group is gone");
    a->ops->connect(a, k_g1);
    CHECK(take(a, ev, 8) == 1 && ev[0].kind == NK_LOBBY_CONNECT_BSSID && is_own_bssid(ev[0].mac) &&
              memcmp(ev[0].mac, bssid, 6) != 0,
          "a group made again gets a new BSSID");
    nk_loopnet_destroy(net);
}

/* send_scan_results: the groups of the scanner's product, newest group first, each with its
 * BSSID, then SCAN_COMPLETE. */
static void test_scan_order(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, A), *b = nk_loopnet_console(net, B),
                     *c = nk_loopnet_console(net, C), *d = nk_loopnet_console(net, D);
    NkLobbyEvent ev[8];
    uint8_t bssid[3][6];
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    login(c, k_c, "Charlie", k_game);
    login(d, k_d, "Delta", k_game);
    /* Made in the order GROUP001, MIKE0003, ALPHA002: name order would be ALPHA, GROUP, MIKE. */
    a->ops->connect(a, k_g1);
    take(a, ev, 8);
    memcpy(bssid[0], ev[0].mac, 6);
    b->ops->connect(b, k_g3);
    take(b, ev, 8);
    memcpy(bssid[1], ev[0].mac, 6);
    c->ops->connect(c, k_g2);
    take(c, ev, 8);
    memcpy(bssid[2], ev[0].mac, 6);
    CHECK(memcmp(bssid[0], bssid[1], 6) != 0 && memcmp(bssid[1], bssid[2], 6) != 0 &&
              memcmp(bssid[0], bssid[2], 6) != 0,
          "each group has its own BSSID");
    nk_loopnet_trace(net, NULL, 0);

    CHECK(d->ops->scan(d) == NK_NET_OK, "scan");
    static const NkLoopTrace t_scan[] = {{D, NK_LOBBY_SCAN, 0}, {D, NK_LOBBY_SCAN, 0},
                                         {D, NK_LOBBY_SCAN, 0}, {D, NK_LOBBY_SCAN_COMPLETE, 0}};
    CHECK(trace_is(net, t_scan, 4, "scan"), "only the scanner hears");
    size_t n = take(d, ev, 8);
    CHECK(n == 4 && memcmp(ev[0].group, k_g2, 8) == 0 && memcmp(ev[0].mac, bssid[2], 6) == 0 &&
              memcmp(ev[1].group, k_g3, 8) == 0 && memcmp(ev[1].mac, bssid[1], 6) == 0 &&
              memcmp(ev[2].group, k_g1, 8) == 0 && memcmp(ev[2].mac, bssid[0], 6) == 0 &&
              ev[3].kind == NK_LOBBY_SCAN_COMPLETE,
          "ALPHA002, MIKE0003, GROUP001 (newest first), each with its BSSID, then complete");

    /* MIKE0003 empties and goes; GROUP001 stays where it was in the list. */
    b->ops->disconnect(b);
    d->ops->scan(d);
    n = take(d, ev, 8);
    CHECK(n == 3 && memcmp(ev[0].group, k_g2, 8) == 0 && memcmp(ev[1].group, k_g1, 8) == 0 &&
              ev[2].kind == NK_LOBBY_SCAN_COMPLETE,
          "an emptied group leaves the list");
    nk_loopnet_destroy(net);
}

/* Groups belong to one product code: the same name in another game is another group. */
static void test_products_apart(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, A), *b = nk_loopnet_console(net, B);
    NkLobbyEvent ev[8];
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_other_game);
    a->ops->connect(a, k_g1);
    take(a, ev, 8);
    b->ops->scan(b);
    CHECK(take(b, ev, 8) == 1 && ev[0].kind == NK_LOBBY_SCAN_COMPLETE, "B cannot see A's game's group");
    b->ops->connect(b, k_g1);
    CHECK(take(b, ev, 8) == 1 && ev[0].kind == NK_LOBBY_CONNECT_BSSID,
          "the same name in another game is a group of its own");
    CHECK(take(a, ev, 8) == 0, "and A is not told");
    nk_loopnet_destroy(net);
}

/* Group names: A-Z, a-z and 0-9 up to the first zero byte, compared up to that byte. */
static void test_group_names(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, A), *b = nk_loopnet_console(net, B);
    NkLobbyEvent ev[8];
    static const uint8_t short_x[8] = {'a', 'b', 0, 'X', 'X', 'X', 'X', 'X'};
    static const uint8_t short_y[8] = {'a', 'b', 0, 'Y', '?', ' ', 0, 0};
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    CHECK(a->ops->connect(a, short_x) == NK_NET_OK, "a short lower-case name");
    take(a, ev, 8);
    CHECK(b->ops->connect(b, short_y) == NK_NET_OK, "bytes after the zero byte are not checked");
    size_t n = take(b, ev, 8);
    CHECK(n == 2 && ev[0].kind == NK_LOBBY_CONNECT && memcmp(ev[0].mac, k_a, 6) == 0,
          "nor compared: B joined A's group");
    nk_loopnet_destroy(net);
}

/* The requests PPSSPP's server answers by logging the client out. */
static void test_protocol_errors(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, A), *b = nk_loopnet_console(net, B);
    NkLobbyEvent ev[8];
    static const uint8_t bad_name[8] = {'B', 'A', 'D', ' ', 'N', 'A', 'M', 'E'};

    /* A join while in a group. */
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    a->ops->connect(a, k_g1);
    b->ops->connect(b, k_g1);
    take(b, ev, 8);
    int sa = a->ops->pdp_open(a, 0x1000);
    nk_loopnet_trace(net, NULL, 0);
    CHECK(a->ops->connect(a, k_g2) == NK_NET_NOT_LOGGED_IN, "a join while in a group logs A out");
    static const NkLoopTrace t_out[] = {{B, NK_LOBBY_DISCONNECT, ID_A}};
    CHECK(trace_is(net, t_out, 1, "join while grouped"), "B hears A go, and nothing else is sent");
    CHECK(take(a, ev, 8) == 2 && ev[0].kind == NK_LOBBY_CONNECT_BSSID && ev[1].kind == NK_LOBBY_CONNECT,
          "A still takes what it was sent before");
    CHECK(a->ops->pdp_send(a, sa, k_b, 0x1000, "x", 1) == NK_NET_NO_SOCKET, "A's sockets closed");
    CHECK(a->ops->scan(a) == NK_NET_NOT_LOGGED_IN, "A is logged out");
    take(b, ev, 8);

    /* A scan inside a group. */
    CHECK(login(a, k_a, "Alpha", k_game) == NK_NET_OK, "A logs in again");
    a->ops->connect(a, k_g1);
    take(a, ev, 8);
    nk_loopnet_trace(net, NULL, 0);
    CHECK(b->ops->scan(b) == NK_NET_NOT_LOGGED_IN, "a scan inside a group logs B out");
    static const NkLoopTrace t_scan[] = {{A, NK_LOBBY_DISCONNECT, ID_B}};
    CHECK(trace_is(net, t_scan, 1, "scan while grouped"), "A hears B go, and B gets no scan");

    /* A leave outside a group. */
    CHECK(login(b, k_b, "Bravo", k_game) == NK_NET_OK, "B logs in again");
    CHECK(b->ops->disconnect(b) == NK_NET_NOT_LOGGED_IN, "a leave outside a group logs B out");
    CHECK(b->ops->scan(b) == NK_NET_NOT_LOGGED_IN, "B is logged out");

    /* A group name with a character outside A-Z, a-z and 0-9. */
    CHECK(login(b, k_b, "Bravo", k_game) == NK_NET_OK, "B logs in again");
    nk_loopnet_trace(net, NULL, 0);
    CHECK(b->ops->connect(b, bad_name) == NK_NET_NOT_LOGGED_IN, "an invalid group name logs B out");
    CHECK(trace_is(net, NULL, 0, "invalid name"), "and makes no group");
    CHECK(b->ops->connect(b, NULL) == NK_NET_NOT_LOGGED_IN, "B is logged out");
    nk_loopnet_destroy(net);
}

static void test_pdp(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0), *b = nk_loopnet_console(net, 1),
                     *c = nk_loopnet_console(net, 2);
    NkLobbyEvent ev[8];
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    login(c, k_c, "Charlie", k_game);
    int sa = a->ops->pdp_open(a, 0x1000), sb = b->ops->pdp_open(b, 0x2000), sc = c->ops->pdp_open(c, 0x2000);
    CHECK(b->ops->pdp_send(b, sb, k_a, 0x1000, "x", 1) == NK_NET_NOT_IN_GROUP, "no group, no send");
    a->ops->connect(a, k_g1);
    b->ops->connect(b, k_g1);
    take(a, ev, 8);
    take(b, ev, 8);

    CHECK(b->ops->pdp_send(b, sb, k_a, 0x1000, "hello", 5) == NK_NET_OK, "B to A");
    uint8_t mac[6], buf[NK_RELAY_PDP_MAX + 1u];
    uint16_t port = 0;
    uint32_t len = 0;
    CHECK(a->ops->pdp_recv(a, sa, mac, &port, buf, sizeof buf, &len) == NK_NET_OK && len == 5 &&
              memcmp(mac, k_b, 6) == 0 && port == 0x2000 && memcmp(buf, "hello", 5) == 0,
          "A gets B's datagram with B's MAC and B's port");

    /* C is not in the group: what B sends it is dropped, and the send still succeeds. */
    CHECK(b->ops->pdp_send(b, sb, k_c, 0x2000, "lost", 4) == NK_NET_OK, "a send outside the group succeeds");
    CHECK(c->ops->pdp_recv(c, sc, mac, &port, buf, sizeof buf, &len) == NK_NET_AGAIN, "and never arrives");
    CHECK(b->ops->pdp_send(b, sb, k_a, 0x1001, "lost", 4) == NK_NET_OK &&
              a->ops->pdp_recv(a, sa, mac, &port, buf, sizeof buf, &len) == NK_NET_AGAIN,
          "a port nobody opened drops the datagram");

    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    CHECK(b->ops->pdp_send(b, sb, bcast, 0x1000, "all", 3) == NK_NET_BAD_ARG,
          "broadcast is the caller's to fan out");
    CHECK(b->ops->pdp_send(b, sb, k_a, 0x1000, buf, NK_RELAY_PDP_MAX + 1u) == NK_NET_TOO_BIG,
          "a datagram over 10 KiB is refused");
    CHECK(b->ops->pdp_send(b, sb, k_a, 0x1000, buf, NK_RELAY_PDP_MAX) == NK_NET_OK &&
              a->ops->pdp_recv(a, sa, mac, &port, buf, sizeof buf, &len) == NK_NET_OK &&
              len == NK_RELAY_PDP_MAX,
          "exactly 10 KiB arrives");
    CHECK(b->ops->pdp_send(b, sb + 1, k_a, 0x1000, "x", 1) == NK_NET_NO_SOCKET, "an unopened handle");

    /* Order, and a full queue: datagrams past NK_NET_PDP_QUEUE are dropped and counted. */
    for (uint32_t i = 0; i < NK_NET_PDP_QUEUE + 3u; i++) {
        uint8_t seq = (uint8_t)i;
        b->ops->pdp_send(b, sb, k_a, 0x1000, &seq, 1);
    }
    CHECK(a->ops->pdp_dropped(a) == 3u, "three dropped at A (got %u)", (unsigned)a->ops->pdp_dropped(a));
    int in_order = 1;
    for (uint32_t i = 0; i < NK_NET_PDP_QUEUE; i++)
        if (a->ops->pdp_recv(a, sa, mac, &port, buf, sizeof buf, &len) != NK_NET_OK || buf[0] != (uint8_t)i)
            in_order = 0;
    CHECK(in_order, "the queued datagrams arrive in the order sent");

    /* Closing a socket discards what waits on it; leaving the group stops sends. */
    b->ops->pdp_send(b, sb, k_a, 0x1000, "x", 1);
    CHECK(a->ops->pdp_close(a, sa) == NK_NET_OK, "close");
    CHECK(a->ops->pdp_recv(a, sa, mac, &port, buf, sizeof buf, &len) == NK_NET_NO_SOCKET, "closed");
    b->ops->disconnect(b);
    CHECK(b->ops->pdp_send(b, sb, k_a, 0x1000, "x", 1) == NK_NET_NOT_IN_GROUP, "after leaving");
    nk_loopnet_destroy(net);
}

/* A console that stops taking its events: as PPSSPP's server logs a failed send and carries on,
 * the medium drops what the full queue cannot hold and counts it; the console stays in the lobby
 * and keeps the events it holds. */
static void test_event_overflow(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0), *b = nk_loopnet_console(net, 1);
    NkLobbyEvent ev[NK_NET_EVENT_QUEUE];
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    b->ops->connect(b, k_g1);
    take(b, ev, NK_NET_EVENT_QUEUE);
    /* Each scan sends A one SCAN and one SCAN_COMPLETE: 32 scans fill the queue exactly. */
    unsigned scans = 0;
    while (scans < NK_NET_EVENT_QUEUE / 2u && a->ops->scan(a) == NK_NET_OK) scans++;
    CHECK(scans == NK_NET_EVENT_QUEUE / 2u && a->ops->events_dropped(a) == 0u,
          "32 scans fit without a drop (%u)", scans);
    nk_loopnet_trace(net, NULL, 0);
    CHECK(a->ops->scan(a) == NK_NET_OK, "the next scan is still served");
    CHECK(a->ops->events_dropped(a) == 2u, "and both its events are dropped and counted (got %u)",
          (unsigned)a->ops->events_dropped(a));
    CHECK(trace_is(net, NULL, 0, "overflow"), "a dropped event is not queued");
    CHECK(take(a, ev, NK_NET_EVENT_QUEUE) == NK_NET_EVENT_QUEUE && ev[0].kind == NK_LOBBY_SCAN &&
              ev[NK_NET_EVENT_QUEUE - 1u].kind == NK_LOBBY_SCAN_COMPLETE,
          "A takes the 64 events it holds, in order");
    CHECK(take(a, NULL, 1) == 0 && take(a, ev, 1) == 0, "take is bounded, and the queue is empty");

    /* A is still logged in: a join works and B hears of it. */
    CHECK(a->ops->connect(a, k_g1) == NK_NET_OK, "A joins");
    CHECK(take(a, ev, NK_NET_EVENT_QUEUE) == 2u && take(b, ev, NK_NET_EVENT_QUEUE) == 1u &&
              ev[0].kind == NK_LOBBY_CONNECT && ev[0].peer_id == ID_A,
          "and the lobby carries on");
    CHECK(a->ops->events_dropped(a) == 2u, "the count stays");
    nk_loopnet_destroy(net);
}

static void test_trace_bound(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0);
    NkLobbyEvent ev[NK_NET_EVENT_QUEUE];
    NkLoopTrace tr[NK_LOOPNET_TRACE];
    login(a, k_a, "Alpha", k_game);
    for (unsigned i = 0; i < NK_LOOPNET_TRACE + 2u; i++) {
        a->ops->scan(a); /* one SCAN_COMPLETE each, no group */
        take(a, ev, NK_NET_EVENT_QUEUE);
    }
    CHECK(nk_loopnet_trace(net, tr, NK_LOOPNET_TRACE) == NK_LOOPNET_TRACE + 2u,
          "the trace counts what it could not keep");
    CHECK(nk_loopnet_trace(net, tr, NK_LOOPNET_TRACE) == 0u, "and starts again empty");
    CHECK(nk_loopnet_trace(NULL, tr, 1) == 0u, "no medium, no trace");
    nk_loopnet_destroy(net);
}

int main(void) {
    test_alone();
    test_login_rules();
    test_join_order();
    test_leave_order();
    test_scan_order();
    test_products_apart();
    test_group_names();
    test_protocol_errors();
    test_pdp();
    test_event_overflow();
    test_trace_bound();
    if (g_failures) {
        fprintf(stderr, "test_nk_adhoc_net: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_adhoc_net: PASS\n");
    return 0;
}
