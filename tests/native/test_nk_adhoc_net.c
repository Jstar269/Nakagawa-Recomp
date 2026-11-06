// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The ad hoc transport seam and its in-process medium (src/core/nk_adhoc_net.c, #1139 phase 2):
 * a console alone, and two to four simulated consoles in one process. The lobby rules checked here
 * are those of the relay server (aemu_postoffice 02abf18f adhocctl server): join order, the
 * leader's MAC, scans per product code, and who hears of a departure. No socket and no network.
 * MACs, names and product codes are synthetic. */

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

/* B's MAC sorts before A's, so join-order and MAC-order differ. */
static const uint8_t k_a[6] = {0x02, 0x50, 0x00, 0x00, 0x00, 0x0a};
static const uint8_t k_b[6] = {0x02, 0x10, 0x00, 0x00, 0x00, 0x0b};
static const uint8_t k_c[6] = {0x02, 0x30, 0x00, 0x00, 0x00, 0x0c};
static const uint8_t k_g1[8] = {'G', 'R', 'O', 'U', 'P', '0', '0', '1'};
static const uint8_t k_g2[8] = {'A', 'L', 'P', 'H', 'A', '0', '0', '2'};
static const char k_game[] = "ULUS10000";
static const char k_other_game[] = "ULES20000";

static int login(NkAdhocTransport *t, const uint8_t *mac, const char *nick, const char *product) {
    return t->ops->login(t, mac, nick, product);
}

static size_t take(NkAdhocTransport *t, NkLobbyEvent *ev, size_t max) {
    return t->ops->take_events(t, ev, max);
}

static void test_alone(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0);
    NkLobbyEvent ev[8];
    CHECK(strcmp(a->ops->name, "loopback") == 0, "transport name");
    CHECK(nk_loopnet_console(net, NK_LOOPNET_CONSOLES) == NULL, "no console past the last");
    CHECK(a->ops->connect(a, k_g1) == NK_NET_NOT_LOGGED_IN, "connect before login is refused");
    CHECK(a->ops->scan(a) == NK_NET_NOT_LOGGED_IN, "scan before login is refused");
    CHECK(a->ops->pdp_open(a, 0x1234) == NK_NET_NOT_LOGGED_IN, "a socket before login is refused");
    CHECK(login(a, k_a, "Alone", k_game) == NK_NET_OK, "login");
    CHECK(login(a, k_a, "Alone", k_game) == NK_NET_BAD_ARG, "a second login is refused");

    CHECK(a->ops->scan(a) == NK_NET_OK, "scan");
    CHECK(take(a, ev, 8) == 1 && ev[0].kind == NK_LOBBY_SCAN_COMPLETE, "an empty scan only completes");

    CHECK(a->ops->connect(a, k_g1) == NK_NET_OK, "create");
    size_t n = take(a, ev, 8);
    CHECK(n == 1 && ev[0].kind == NK_LOBBY_CONNECT_BSSID && memcmp(ev[0].mac, k_a, 6) == 0,
          "a creator alone hears only that it leads the group (got %zu events)", n);

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

static void test_join_order(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0), *b = nk_loopnet_console(net, 1),
                     *c = nk_loopnet_console(net, 2), *d = nk_loopnet_console(net, 3);
    NkLobbyEvent ev[8];
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    login(c, k_c, "Charlie", k_game);
    login(d, (const uint8_t[]){0x02, 0x40, 0, 0, 0, 0x0d}, "Delta", k_other_game);

    a->ops->connect(a, k_g1);
    take(a, ev, 8);
    b->ops->connect(b, k_g1);
    size_t n = take(b, ev, 8);
    CHECK(n == 2 && ev[0].kind == NK_LOBBY_CONNECT && memcmp(ev[0].mac, k_a, 6) == 0 &&
              strcmp(ev[0].nickname, "Alpha") == 0 && ev[0].peer_id == 1u,
          "the joiner hears of the member first");
    CHECK(n == 2 && ev[1].kind == NK_LOBBY_CONNECT_BSSID && memcmp(ev[1].mac, k_a, 6) == 0,
          "then the leader's MAC, which is the creator's");
    n = take(a, ev, 8);
    CHECK(n == 1 && ev[0].kind == NK_LOBBY_CONNECT && memcmp(ev[0].mac, k_b, 6) == 0 &&
              ev[0].peer_id == 2u,
          "the member hears of the joiner");

    /* C joins A and B: it hears of them in MAC order (B 02:10 before A 02:50), not join order. */
    c->ops->connect(c, k_g1);
    n = take(c, ev, 8);
    CHECK(n == 3 && memcmp(ev[0].mac, k_b, 6) == 0 && memcmp(ev[1].mac, k_a, 6) == 0 &&
              ev[2].kind == NK_LOBBY_CONNECT_BSSID && memcmp(ev[2].mac, k_a, 6) == 0,
          "members in MAC order, then the leader");
    CHECK(take(a, ev, 8) == 1 && take(b, ev, 8) == 1, "each member hears of C once");

    /* D runs another game: it cannot see the group, and its own group of that name is apart. */
    d->ops->scan(d);
    CHECK(take(d, ev, 8) == 1 && ev[0].kind == NK_LOBBY_SCAN_COMPLETE, "groups are per product code");
    d->ops->connect(d, k_g1);
    n = take(d, ev, 8);
    CHECK(n == 1 && ev[0].kind == NK_LOBBY_CONNECT_BSSID && ev[0].mac[1] == 0x40,
          "the same name in another game is a group of its own");
    CHECK(take(a, ev, 8) == 0, "and its members are not told");

    /* The leader leaves: the rest are told, and the group keeps its creator as leader. */
    a->ops->disconnect(a);
    CHECK(take(a, ev, 8) == 0, "the leaver hears nothing");
    n = take(b, ev, 8);
    CHECK(n == 1 && ev[0].kind == NK_LOBBY_DISCONNECT && ev[0].peer_id == 1u, "B hears A leave");
    CHECK(take(c, ev, 8) == 1 && ev[0].kind == NK_LOBBY_DISCONNECT, "C hears A leave");
    a->ops->scan(a);
    n = take(a, ev, 8);
    CHECK(n == 2 && ev[0].kind == NK_LOBBY_SCAN && memcmp(ev[0].group, k_g1, 8) == 0 &&
              memcmp(ev[0].mac, k_a, 6) == 0 && ev[1].kind == NK_LOBBY_SCAN_COMPLETE,
          "the group still names its creator as leader");
    nk_loopnet_destroy(net);
}

static void test_scan_and_leave(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0), *b = nk_loopnet_console(net, 1),
                     *c = nk_loopnet_console(net, 2);
    NkLobbyEvent ev[8];
    login(a, k_a, "Alpha", k_game);
    login(b, k_b, "Bravo", k_game);
    login(c, k_c, "Charlie", k_game);
    CHECK(login(nk_loopnet_console(net, 3), k_a, "Copy", k_game) == NK_NET_BAD_ARG,
          "a MAC already logged in is refused");
    a->ops->connect(a, k_g1);
    b->ops->connect(b, k_g2);
    take(a, ev, 8);
    take(b, ev, 8);
    c->ops->scan(c);
    size_t n = take(c, ev, 8);
    CHECK(n == 3 && memcmp(ev[0].group, k_g2, 8) == 0 && memcmp(ev[0].mac, k_b, 6) == 0 &&
              memcmp(ev[1].group, k_g1, 8) == 0 && memcmp(ev[1].mac, k_a, 6) == 0 &&
              ev[2].kind == NK_LOBBY_SCAN_COMPLETE,
          "a scan lists every group in name order, each with its leader");

    /* A join leaves the current group first: B moves from ALPHA002 to GROUP001. */
    b->ops->connect(b, k_g1);
    n = take(b, ev, 8);
    CHECK(n == 2 && ev[0].kind == NK_LOBBY_CONNECT && ev[1].kind == NK_LOBBY_CONNECT_BSSID,
          "B joins GROUP001");
    c->ops->scan(c);
    n = take(c, ev, 8);
    CHECK(n == 2 && memcmp(ev[0].group, k_g1, 8) == 0, "the group B left alone is gone");

    /* Logging out leaves the group too. */
    a->ops->logout(a);
    CHECK(take(b, ev, 8) == 1 && ev[0].kind == NK_LOBBY_DISCONNECT && ev[0].peer_id == 1u,
          "B hears A log out");
    CHECK(a->ops->disconnect(a) == NK_NET_NOT_LOGGED_IN, "after logout the lobby is closed");
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

static void test_event_overflow(void) {
    NkAdhocLoopNet *net = nk_loopnet_create();
    NkAdhocTransport *a = nk_loopnet_console(net, 0);
    NkLobbyEvent ev[1];
    login(a, k_a, "Alpha", k_game);
    for (unsigned i = 0; i < NK_NET_EVENT_QUEUE + 2u; i++) a->ops->scan(a);
    CHECK(a->ops->events_dropped(a) == 2u, "events past the queue are counted, not lost silently");
    CHECK(take(a, ev, 1) == 1 && take(a, NULL, 1) == 0, "take is bounded by the caller");
    nk_loopnet_destroy(net);
}

int main(void) {
    test_alone();
    test_join_order();
    test_scan_and_leave();
    test_pdp();
    test_event_overflow();
    if (g_failures) {
        fprintf(stderr, "test_nk_adhoc_net: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_adhoc_net: PASS\n");
    return 0;
}
