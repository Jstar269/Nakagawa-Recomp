/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/* In-process TCP loopback test for NkAdhocRelayClient: starts an ephemeral 127.0.0.1 test server
 * speaking aemu_postoffice lobby + data relay frames, verifies two clients meet, exchange PDP
 * and PTP traffic, consume canned CHAT frames cleanly, and fail closed on bad server frames. */

#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "nk_adhoc_relay_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET TSock;
#  define T_INV INVALID_SOCKET
#  define TSND 0
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int TSock;
#  define T_INV (-1)
#  ifdef MSG_NOSIGNAL
#    define TSND MSG_NOSIGNAL
#  else
#    define TSND 0
#  endif
#endif

static int g_failures;

static void check_fn(int cond, const char *fmt, ...) {
    if (cond) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_failures++;
}
#define CHECK(cond, ...) check_fn((cond) != 0, __VA_ARGS__)

static void t_close(TSock s) {
    if (s == T_INV) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static bool t_would_block(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

static void t_nonblock(TSock s) {
#ifdef _WIN32
    u_long on = 1u;
    ioctlsocket(s, FIONBIO, &on);
#else
    int f = fcntl(s, F_GETFL, 0);
    if (f >= 0) fcntl(s, F_SETFL, f | O_NONBLOCK);
#endif
}

static TSock t_listen_loopback(uint16_t *out_port) {
    TSock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == T_INV) return T_INV;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0u;
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(s, 16) != 0) {
        t_close(s);
        return T_INV;
    }
    socklen_t len = (socklen_t)sizeof sa;
    if (getsockname(s, (struct sockaddr *)&sa, &len) != 0) {
        t_close(s);
        return T_INV;
    }
    t_nonblock(s);
    *out_port = ntohs(sa.sin_port);
    return s;
}

static void put16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static uint32_t get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---- minimal test relay server (in-process, synchronous pump) ------------------------------ */

#define MAX_SRV_CONNS 24

typedef struct LobbyPeer {
    TSock fd;
    int active;
    int logged_in;
    int in_group;
    uint32_t peer_id;
    uint8_t mac[6];
    char nick[128];
    char game[9];
    uint8_t group[8];
    uint8_t rx[2048];
    size_t rx_len;
} LobbyPeer;

typedef struct DataConn {
    TSock fd;
    int active;
    int kind; /* -1 init pending, 0 pdp, 1 ptp_listen, 2 ptp_connect, 3 ptp_accept, 4 ptp_pair */
    uint8_t src_mac[6];
    uint16_t sport;
    uint8_t dst_mac[6];
    uint16_t dport;
    int partner;
    uint8_t rx[65536];
    size_t rx_len;
} DataConn;

typedef struct TestServer {
    TSock lobby_listen;
    TSock data_listen;
    uint16_t lobby_port;
    uint16_t data_port;
    LobbyPeer peers[8];
    DataConn conns[MAX_SRV_CONNS];
    uint8_t group_bssid[6];
    uint8_t active_group[8];
    int group_exists;
    int ping_count;
} TestServer;

static bool t_send_all(TSock fd, const uint8_t *buf, size_t n) {
    size_t off = 0u;
    while (off < n) {
#ifdef _WIN32
        int w = send(fd, (const char *)(buf + off), (int)(n - off), TSND);
#else
        ssize_t w = send(fd, buf + off, n - off, TSND);
#endif
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && t_would_block()) continue;
        return false;
    }
    return true;
}

static size_t t_drain(TSock fd, uint8_t *buf, size_t len, size_t cap, int *closed) {
    while (len < cap) {
#ifdef _WIN32
        int r = recv(fd, (char *)(buf + len), (int)(cap - len), 0);
#else
        ssize_t r = recv(fd, buf + len, cap - len, 0);
#endif
        if (r > 0) {
            len += (size_t)r;
            continue;
        }
        if (r == 0) {
            if (closed) *closed = 1;
            break;
        }
        if (t_would_block()) break;
        if (closed) *closed = 1;
        break;
    }
    return len;
}

static void srv_init(TestServer *s) {
    memset(s, 0, sizeof *s);
    s->lobby_listen = t_listen_loopback(&s->lobby_port);
    s->data_listen = t_listen_loopback(&s->data_port);
    for (int i = 0; i < 8; i++) s->peers[i].fd = T_INV;
    for (int i = 0; i < MAX_SRV_CONNS; i++) {
        s->conns[i].fd = T_INV;
        s->conns[i].kind = -1;
        s->conns[i].partner = -1;
    }
    static const uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(s->group_bssid, bssid, 6);
}

static void srv_destroy(TestServer *s) {
    t_close(s->lobby_listen);
    t_close(s->data_listen);
    for (int i = 0; i < 8; i++) t_close(s->peers[i].fd);
    for (int i = 0; i < MAX_SRV_CONNS; i++) t_close(s->conns[i].fd);
}

static void srv_send_s2c_connect(LobbyPeer *to, const LobbyPeer *about) {
    uint8_t pkt[139];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 0x02u;
    memcpy(pkt + 1, about->nick, 128);
    memcpy(pkt + 129, about->mac, 6);
    put32le(pkt + 135, about->peer_id);
    t_send_all(to->fd, pkt, sizeof pkt);
}

static void srv_send_s2c_bssid(LobbyPeer *to, const uint8_t bssid[6]) {
    uint8_t pkt[7];
    pkt[0] = 0x06u;
    memcpy(pkt + 1, bssid, 6);
    t_send_all(to->fd, pkt, sizeof pkt);
}

static void srv_send_s2c_disconnect(LobbyPeer *to, uint32_t peer_id) {
    uint8_t pkt[5];
    pkt[0] = 0x03u;
    put32le(pkt + 1, peer_id);
    t_send_all(to->fd, pkt, sizeof pkt);
}

static void srv_pump(TestServer *s) {
    for (;;) {
        TSock cli = accept(s->lobby_listen, NULL, NULL);
        if (cli == T_INV) break;
        t_nonblock(cli);
        int placed = 0;
        for (int i = 0; i < 8; i++) {
            if (!s->peers[i].active) {
                memset(&s->peers[i], 0, sizeof s->peers[i]);
                s->peers[i].fd = cli;
                s->peers[i].active = 1;
                s->peers[i].peer_id = (uint32_t)(i + 1);
                placed = 1;
                break;
            }
        }
        if (!placed) t_close(cli);
    }
    for (;;) {
        TSock cli = accept(s->data_listen, NULL, NULL);
        if (cli == T_INV) break;
        t_nonblock(cli);
        int placed = 0;
        for (int i = 0; i < MAX_SRV_CONNS; i++) {
            if (!s->conns[i].active) {
                memset(&s->conns[i], 0, sizeof s->conns[i]);
                s->conns[i].fd = cli;
                s->conns[i].active = 1;
                s->conns[i].kind = -1;
                s->conns[i].partner = -1;
                placed = 1;
                break;
            }
        }
        if (!placed) t_close(cli);
    }

    for (int i = 0; i < 8; i++) {
        LobbyPeer *p = &s->peers[i];
        if (!p->active) continue;
        int closed = 0;
        p->rx_len = t_drain(p->fd, p->rx, p->rx_len, sizeof p->rx, &closed);
        while (p->rx_len > 0u) {
            uint8_t op = p->rx[0];
            size_t need = 0u;
            if (op == 0x00u || op == 0x03u || op == 0x04u) need = NK_LOBBY_BARE_SIZE;
            else if (op == 0x01u) need = NK_LOBBY_LOGIN_SIZE;
            else if (op == 0x02u) need = NK_LOBBY_CONNECT_SIZE;
            else {
                p->rx_len = 0u;
                break;
            }
            if (p->rx_len < need) break;
            if (op == 0x00u) {
                s->ping_count++;
            } else if (op == 0x01u) {
                memcpy(p->mac, p->rx + 1, 6);
                memcpy(p->nick, p->rx + 7, 128);
                memcpy(p->game, p->rx + 135, 9);
                p->logged_in = 1;
            } else if (op == 0x02u) {
                memcpy(p->group, p->rx + 1, 8);
                if (!s->group_exists) {
                    s->group_exists = 1;
                    memcpy(s->active_group, p->group, 8);
                }
                for (int j = 0; j < 8; j++) {
                    if (s->peers[j].active && s->peers[j].in_group &&
                        memcmp(s->peers[j].group, p->group, 8) == 0) {
                        srv_send_s2c_connect(p, &s->peers[j]);
                        srv_send_s2c_connect(&s->peers[j], p);
                    }
                }
                p->in_group = 1;
                srv_send_s2c_bssid(p, s->group_bssid);
            } else if (op == 0x03u) {
                if (p->in_group) {
                    p->in_group = 0;
                    for (int j = 0; j < 8; j++) {
                        if (j != i && s->peers[j].active && s->peers[j].in_group)
                            srv_send_s2c_disconnect(&s->peers[j], p->peer_id);
                    }
                    int any_left = 0;
                    for (int j = 0; j < 8; j++) {
                        if (s->peers[j].active && s->peers[j].in_group) any_left = 1;
                    }
                    if (!any_left) s->group_exists = 0;
                }
            } else if (op == 0x04u) {
                /* Inject a canned CHAT packet ahead of SCAN to ensure chat frames are consumed. */
                uint8_t chat[193];
                memset(chat, 0, sizeof chat);
                chat[0] = 0x07u;
                memcpy(chat + 1, "ignored-chat", 12);
                memcpy(chat + 65, "Sys", 3);
                t_send_all(p->fd, chat, sizeof chat);
                if (s->group_exists) {
                    uint8_t scan[15];
                    scan[0] = 0x04u;
                    memcpy(scan + 1, s->active_group, 8);
                    memcpy(scan + 9, s->group_bssid, 6);
                    t_send_all(p->fd, scan, sizeof scan);
                }
                uint8_t done = 0x05u;
                t_send_all(p->fd, &done, 1u);
            }
            if (need < p->rx_len) memmove(p->rx, p->rx + need, p->rx_len - need);
            p->rx_len -= need;
        }
        if (closed) {
            if (p->in_group) {
                for (int j = 0; j < 8; j++) {
                    if (j != i && s->peers[j].active && s->peers[j].in_group)
                        srv_send_s2c_disconnect(&s->peers[j], p->peer_id);
                }
            }
            t_close(p->fd);
            p->fd = T_INV;
            p->active = 0;
        }
    }

    for (int i = 0; i < MAX_SRV_CONNS; i++) {
        DataConn *dc = &s->conns[i];
        if (!dc->active) continue;
        int closed = 0;
        dc->rx_len = t_drain(dc->fd, dc->rx, dc->rx_len, sizeof dc->rx, &closed);
        if (dc->kind < 0 && dc->rx_len >= 24u) {
            dc->kind = (int)get32le(dc->rx);
            memcpy(dc->src_mac, dc->rx + 4, 6);
            dc->sport = get16le(dc->rx + 12);
            memcpy(dc->dst_mac, dc->rx + 14, 6);
            dc->dport = get16le(dc->rx + 22);
            if (dc->rx_len > 24u) memmove(dc->rx, dc->rx + 24, dc->rx_len - 24u);
            dc->rx_len -= 24u;

            if (dc->kind == 2) { /* PTP_CONNECT: notify matching listener */
                for (int j = 0; j < MAX_SRV_CONNS; j++) {
                    DataConn *ls = &s->conns[j];
                    if (ls->active && ls->kind == 1 &&
                        memcmp(ls->src_mac, dc->dst_mac, 6) == 0 && ls->sport == dc->dport) {
                        uint8_t note[10];
                        memset(note, 0, sizeof note);
                        memcpy(note, dc->src_mac, 6);
                        put16le(note + 8, dc->sport);
                        t_send_all(ls->fd, note, sizeof note);
                        break;
                    }
                }
            } else if (dc->kind == 3) { /* PTP_ACCEPT: pair with PTP_CONNECT and ack both */
                for (int j = 0; j < MAX_SRV_CONNS; j++) {
                    DataConn *cl = &s->conns[j];
                    if (cl->active && cl->kind == 2 &&
                        memcmp(cl->src_mac, dc->dst_mac, 6) == 0 && cl->sport == dc->dport &&
                        memcmp(cl->dst_mac, dc->src_mac, 6) == 0 && cl->dport == dc->sport) {
                        uint8_t ack_cl[10], ack_ac[10];
                        memset(ack_cl, 0, sizeof ack_cl);
                        memset(ack_ac, 0, sizeof ack_ac);
                        memcpy(ack_cl, dc->src_mac, 6);
                        put16le(ack_cl + 8, dc->sport);
                        memcpy(ack_ac, cl->src_mac, 6);
                        put16le(ack_ac + 8, cl->sport);
                        t_send_all(cl->fd, ack_cl, sizeof ack_cl);
                        t_send_all(dc->fd, ack_ac, sizeof ack_ac);
                        cl->kind = 4;
                        dc->kind = 4;
                        cl->partner = i;
                        dc->partner = j;
                        break;
                    }
                }
            }
        }
        if (dc->kind == 0) { /* PDP forward */
            while (dc->rx_len >= 14u) {
                uint32_t plen = get32le(dc->rx + 10);
                if (dc->rx_len < 14u + plen) break;
                uint8_t dst[6];
                memcpy(dst, dc->rx, 6);
                uint16_t dp = get16le(dc->rx + 8);
                for (int j = 0; j < MAX_SRV_CONNS; j++) {
                    DataConn *dst_c = &s->conns[j];
                    if (dst_c->active && dst_c->kind == 0 &&
                        memcmp(dst_c->src_mac, dst, 6) == 0 && dst_c->sport == dp) {
                        uint8_t hdr[14];
                        memset(hdr, 0, sizeof hdr);
                        memcpy(hdr, dc->src_mac, 6);
                        put16le(hdr + 8, dc->sport);
                        put32le(hdr + 10, plen);
                        t_send_all(dst_c->fd, hdr, sizeof hdr);
                        if (plen) t_send_all(dst_c->fd, dc->rx + 14, plen);
                    }
                }
                const size_t total = 14u + plen;
                if (total < dc->rx_len) memmove(dc->rx, dc->rx + total, dc->rx_len - total);
                dc->rx_len -= total;
            }
        } else if (dc->kind == 4 && dc->partner >= 0 && dc->rx_len > 0u) {
            DataConn *pt = &s->conns[dc->partner];
            if (pt->active) t_send_all(pt->fd, dc->rx, dc->rx_len);
            dc->rx_len = 0u;
        }
        if (closed) {
            if (dc->kind == 4 && dc->partner >= 0 && s->conns[dc->partner].active) {
                t_close(s->conns[dc->partner].fd);
                s->conns[dc->partner].fd = T_INV;
                s->conns[dc->partner].active = 0;
            }
            t_close(dc->fd);
            dc->fd = T_INV;
            dc->active = 0;
        }
    }
}

static size_t pump_take(TestServer *s, NkAdhocTransport *a, NkAdhocTransport *b,
                        NkAdhocTransport *who, NkLobbyEvent *out, size_t cap) {
    size_t n = 0u;
    for (int step = 0; step < 50 && n < cap; step++) {
        srv_pump(s);
        if (a) a->ops->pump(a);
        if (b) b->ops->pump(b);
        n += who->ops->take_events(who, out + n, cap - n);
        if (n > 0u) break;
    }
    return n;
}

static void test_two_clients_loopback(void) {
    TestServer srv;
    srv_init(&srv);
    CHECK(srv.lobby_listen != T_INV && srv.data_listen != T_INV, "loopback server binds");

    NkAdhocRelayConfig cfg = {"127.0.0.1", srv.lobby_port, srv.data_port, NK_RELAY_PORT_OFFSET};
    NkAdhocRelayClient *ca = nk_relay_client_create(&cfg);
    NkAdhocRelayClient *cb = nk_relay_client_create(&cfg);
    CHECK(ca != NULL && cb != NULL, "both relay clients created");
    NkAdhocTransport *ta = nk_relay_client_transport(ca);
    NkAdhocTransport *tb = nk_relay_client_transport(cb);

    static const uint8_t mac_a[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
    static const uint8_t mac_b[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x02};
    static const uint8_t grp[8] = {'M', 'H', 'F', 'U', '0', '0', '1', 0};

    CHECK(ta->ops->login(ta, mac_a, "HunterA", "ULUS10391") == NK_NET_OK, "A login queued");
    CHECK(tb->ops->login(tb, mac_b, "HunterB", "ULUS10391") == NK_NET_OK, "B login queued");
    for (int i = 0; i < 15; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
        tb->ops->pump(tb);
    }
    CHECK(nk_relay_client_lobby_connected(ca) && nk_relay_client_lobby_connected(cb),
          "both clients connected over loopback TCP");

    CHECK(ta->ops->connect(ta, grp) == NK_NET_OK, "A creates group");
    NkLobbyEvent ev[8];
    size_t na = pump_take(&srv, ta, tb, ta, ev, 8);
    CHECK(na == 1u && ev[0].kind == NK_LOBBY_CONNECT_BSSID, "A receives group BSSID");

    CHECK(tb->ops->scan(tb) == NK_NET_OK, "B scans groups");
    size_t nb = 0u;
    for (int step = 0; step < 50 && nb < 2u; step++) {
        nb += pump_take(&srv, ta, tb, tb, ev + nb, 8 - nb);
    }
    CHECK(nb == 2u && ev[0].kind == NK_LOBBY_SCAN && memcmp(ev[0].group, grp, 8) == 0 &&
              ev[1].kind == NK_LOBBY_SCAN_COMPLETE,
          "B receives SCAN + SCAN_COMPLETE and consumes canned CHAT cleanly");

    CHECK(tb->ops->connect(tb, grp) == NK_NET_OK, "B joins A's group");
    na = 0u;
    for (int step = 0; step < 50 && na < 1u; step++) na += pump_take(&srv, ta, tb, ta, ev, 8);
    CHECK(na == 1u && ev[0].kind == NK_LOBBY_CONNECT &&
              strcmp(ev[0].nickname, "HunterB") == 0 && memcmp(ev[0].mac, mac_b, 6) == 0,
          "A learns HunterB joined");
    nb = 0u;
    for (int step = 0; step < 50 && nb < 2u; step++) nb += pump_take(&srv, ta, tb, tb, ev + nb, 8 - nb);
    CHECK(nb == 2u && ev[0].kind == NK_LOBBY_CONNECT &&
              strcmp(ev[0].nickname, "HunterA") == 0 && ev[1].kind == NK_LOBBY_CONNECT_BSSID,
          "B receives existing member HunterA then BSSID");

    /* PDP exchange across relay with +10000 port offset */
    const int pa = ta->ops->pdp_open(ta, 0x1234u);
    const int pb = tb->ops->pdp_open(tb, 0x2345u);
    CHECK(pa >= 0 && pb >= 0, "both PDP relay streams open");
    for (int i = 0; i < 15; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
        tb->ops->pump(tb);
    }
    CHECK(ta->ops->pdp_send(ta, pa, mac_b, 0x2345u, "quest-ping", 10u) == NK_NET_OK,
          "A sends PDP datagram to B");
    uint8_t rmac[6] = {0};
    uint16_t rport = 0u;
    uint32_t rlen = 0u;
    uint8_t buf[64];
    int got_pdp = NK_NET_AGAIN;
    for (int i = 0; i < 40 && got_pdp == NK_NET_AGAIN; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
        tb->ops->pump(tb);
        got_pdp = tb->ops->pdp_recv(tb, pb, rmac, &rport, buf, sizeof buf, &rlen);
    }
    CHECK(got_pdp == NK_NET_OK && rlen == 10u && memcmp(buf, "quest-ping", 10) == 0 &&
              memcmp(rmac, mac_a, 6) == 0 && rport == 0x1234u,
          "B receives PDP datagram with unshifted port and sender MAC");

    /* PTP listen / connect / accept / bidirectional stream / close */
    const int ls = ta->ops->ptp_listen(ta, 0x3000u, 2);
    CHECK(ls >= 0, "A opens PTP listener");
    for (int i = 0; i < 15; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
    }
    const int cli = tb->ops->ptp_open(tb, 0x4001u, mac_a, 0x3000u);
    CHECK(cli >= 0, "B initiates PTP connect");
    int cstat = 0;
    for (int i = 0; i < 60; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
        tb->ops->pump(tb);
        cstat = tb->ops->ptp_connect_poll(tb, cli);
        if (cstat != 0) break;
    }
    CHECK(cstat == 1, "B's PTP connect completes once A eagerly accepts into backlog");
    int srv_ptp = NK_NET_AGAIN;
    for (int i = 0; i < 30 && srv_ptp == NK_NET_AGAIN; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
        srv_ptp = ta->ops->ptp_accept(ta, ls, rmac, &rport);
    }
    CHECK(srv_ptp >= 0 && memcmp(rmac, mac_b, 6) == 0 && rport == 0x4001u,
          "A accepts connected PTP peer from backlog");
    uint32_t sent = 0u;
    CHECK(tb->ops->ptp_send(tb, cli, "stream-payload", 14u, &sent) == NK_NET_OK && sent == 14u,
          "B sends PTP payload");
    int rrc = NK_NET_AGAIN;
    for (int i = 0; i < 40 && rrc == NK_NET_AGAIN; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
        tb->ops->pump(tb);
        rrc = ta->ops->ptp_recv(ta, srv_ptp, buf, sizeof buf, &rlen);
    }
    CHECK(rrc == NK_NET_OK && rlen == 14u && memcmp(buf, "stream-payload", 14) == 0,
          "A receives PTP payload cleanly");

    tb->ops->ptp_close(tb, cli);
    for (int i = 0; i < 20; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
    }
    CHECK(ta->ops->ptp_is_dead(ta, srv_ptp), "A observes peer PTP close");
    ta->ops->ptp_close(ta, srv_ptp);
    ta->ops->ptp_close(ta, ls);
    ta->ops->pdp_close(ta, pa);
    tb->ops->pdp_close(tb, pb);
    ta->ops->logout(ta);
    tb->ops->logout(tb);
    nk_relay_client_destroy(ca);
    nk_relay_client_destroy(cb);
    srv_destroy(&srv);
}

static void test_bad_server_frames_fail_closed(void) {
    NkAdhocRelayConfig bad = {"", 0, 0, NK_RELAY_PORT_OFFSET};
    CHECK(nk_relay_client_create(&bad) == NULL, "empty host rejected");
    CHECK(nk_relay_client_create(NULL) == NULL, "NULL config rejected");

    uint16_t lp = 0u, dp = 0u;
    TSock ls = t_listen_loopback(&lp);
    TSock ds = t_listen_loopback(&dp);
    NkAdhocRelayConfig cfg = {"127.0.0.1", lp, dp, NK_RELAY_PORT_OFFSET};
    NkAdhocRelayClient *c = nk_relay_client_create(&cfg);
    CHECK(c != NULL, "client created for hostile frame test");
    NkAdhocTransport *t = nk_relay_client_transport(c);
    static const uint8_t mac[6] = {0x02, 1, 2, 3, 4, 5};
    CHECK(t->ops->login(t, mac, "Tester", "ULUS10000") == NK_NET_OK, "login queued");
    TSock accepted = T_INV;
    for (int i = 0; i < 40 && accepted == T_INV; i++) {
        accepted = accept(ls, NULL, NULL);
        t->ops->pump(t);
    }
    CHECK(accepted != T_INV, "hostile test server accepted lobby client");
    /* Send forbidden S2C opcode 0x01 (LOGIN) -- client must mark lobby dead cleanly. */
    const uint8_t bad_op = 0x01u;
    t_send_all(accepted, &bad_op, 1u);
    for (int i = 0; i < 20; i++) t->ops->pump(t);
    CHECK(!nk_relay_client_lobby_connected(c), "lobby session fails closed on invalid opcode");
    const uint8_t grp[8] = {'T', 'E', 'S', 'T', '0', '0', '1', 0};
    CHECK(t->ops->connect(t, grp) == NK_NET_DEAD, "subsequent connect refused after dead lobby");
    t_close(accepted);

    /* Open PDP channel and inject oversized (> NK_RELAY_PDP_MAX) frame: client must close/dead. */
    t_close(ls);
    ls = t_listen_loopback(&lp);
    cfg.lobby_port = lp;
    NkAdhocRelayClient *c2 = nk_relay_client_create(&cfg);
    CHECK(c2 != NULL, "second client created for PDP overflow test");
    NkAdhocTransport *t2 = nk_relay_client_transport(c2);
    CHECK(t2->ops->login(t2, mac, "Tester2", "ULUS10000") == NK_NET_OK, "c2 login");
    TSock acc_lobby = T_INV, acc_data = T_INV;
    const int pdp = t2->ops->pdp_open(t2, 0x1000u);
    CHECK(pdp >= 0, "c2 pdp opened");
    for (int i = 0; i < 60 && (acc_lobby == T_INV || acc_data == T_INV); i++) {
        if (acc_lobby == T_INV) acc_lobby = accept(ls, NULL, NULL);
        if (acc_data == T_INV) acc_data = accept(ds, NULL, NULL);
        t2->ops->pump(t2);
    }
    CHECK(acc_data != T_INV, "hostile server accepted data channel");
    uint8_t bad_pdp[NK_RELAY_PDP_HEADER_SIZE];
    memset(bad_pdp, 0, sizeof bad_pdp);
    memcpy(bad_pdp, mac, 6);
    put16le(bad_pdp + 8, nk_relay_port_out(0x1000u, NK_RELAY_PORT_OFFSET));
    put32le(bad_pdp + 10, NK_RELAY_PDP_MAX + 1u);
    t_send_all(acc_data, bad_pdp, sizeof bad_pdp);
    for (int i = 0; i < 20; i++) t2->ops->pump(t2);
    uint8_t out_mac[6], out_buf[16];
    uint16_t out_port = 0u;
    uint32_t out_len = 0u;
    CHECK(t2->ops->pdp_recv(t2, pdp, out_mac, &out_port, out_buf, sizeof out_buf, &out_len) ==
              NK_NET_DEAD,
          "oversized PDP header fails closed cleanly with NK_NET_DEAD");
    t_close(acc_lobby);
    t_close(acc_data);
    t_close(ls);
    t_close(ds);
    nk_relay_client_destroy(c2);
    nk_relay_client_destroy(c);
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif
    test_two_clients_loopback();
    test_bad_server_frames_fail_closed();
#ifdef _WIN32
    WSACleanup();
#endif
    if (g_failures) {
        fprintf(stderr, "test_nk_adhoc_relay_client: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_adhoc_relay_client: PASS\n");
    return 0;
}
