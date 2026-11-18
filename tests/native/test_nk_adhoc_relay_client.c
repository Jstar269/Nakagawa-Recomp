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
#  include <windows.h>
typedef SOCKET TSock;
#  define T_INV INVALID_SOCKET
#  define TSND 0
static void test_sleep_ms(unsigned ms) { Sleep((DWORD)ms); }
static uint64_t test_now_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <pthread.h>
#  include <sys/resource.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <time.h>
#  include <unistd.h>
typedef int TSock;
#  define T_INV (-1)
#  ifdef MSG_NOSIGNAL
#    define TSND MSG_NOSIGNAL
#  else
#    define TSND 0
#  endif
static void test_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    (void)nanosleep(&ts, NULL);
}
static uint64_t test_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)(ts.tv_nsec / 1000000L);
}
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
    /* Disable Nagle on the in-process loopback server sockets so small sequential
     * lobby/data frames flush immediately on POSIX without waiting for delayed ACKs. */
    int nodelay = 1;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
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

static void test_ptp_accept_waits_for_free_slot(void) {
    TestServer srv;
    srv_init(&srv);
    NkAdhocRelayConfig cfg_a = {"127.0.0.1", srv.lobby_port, srv.data_port, NK_RELAY_PORT_OFFSET};
    NkAdhocRelayConfig cfg_b = {"127.0.0.1", srv.lobby_port, srv.data_port, NK_RELAY_PORT_OFFSET};
    NkAdhocRelayClient *ca = nk_relay_client_create(&cfg_a);
    NkAdhocRelayClient *cb = nk_relay_client_create(&cfg_b);
    CHECK(ca && cb, "slot-wait: clients created");
    NkAdhocTransport *ta = nk_relay_client_transport(ca);
    NkAdhocTransport *tb = nk_relay_client_transport(cb);
    static const uint8_t mac_a[6] = {0x02, 0xaa, 0xbb, 0x01, 0x00, 0x01};
    static const uint8_t mac_b[6] = {0x02, 0xaa, 0xbb, 0x01, 0x00, 0x02};
    static const uint8_t grp[8] = {'S', 'L', 'O', 'T', 'T', 'E', 'S', 'T'};
    CHECK(ta->ops->login(ta, mac_a, "Alpha", "ULUS10001") == NK_NET_OK &&
          tb->ops->login(tb, mac_b, "Beta", "ULUS10001") == NK_NET_OK,
          "slot-wait: both logged in");
    for (int i = 0; i < 30; i++) { srv_pump(&srv); ta->ops->pump(ta); tb->ops->pump(tb); }
    CHECK(ta->ops->connect(ta, grp) == NK_NET_OK && tb->ops->connect(tb, grp) == NK_NET_OK,
          "slot-wait: joined group");
    for (int i = 0; i < 30; i++) { srv_pump(&srv); ta->ops->pump(ta); tb->ops->pump(tb); }

    const int main_lsn = ta->ops->ptp_listen(ta, 0x4001u, 4);
    CHECK(main_lsn >= 0, "slot-wait: main PTP listener opened");
    int fillers[15];
    int nfill = 0;
    for (int i = 0; i < 15; i++) {
        fillers[i] = ta->ops->ptp_listen(ta, (uint16_t)(0x4100u + i), 1);
        if (fillers[i] >= 0) nfill++;
    }
    CHECK(nfill == 15, "slot-wait: filled all 16 PTP slots on console A");
    for (int i = 0; i < 20; i++) { srv_pump(&srv); ta->ops->pump(ta); }

    const int cli = tb->ops->ptp_open(tb, 0x4009u, mac_a, 0x4001u);
    CHECK(cli >= 0, "slot-wait: console B started PTP connect while A slots full");
    for (int i = 0; i < 25; i++) { srv_pump(&srv); ta->ops->pump(ta); tb->ops->pump(tb); }
    /* Still full: nothing consumed/lost, so accept cannot return a child yet. */
    uint8_t pmac[6] = {0};
    uint16_t pport = 0u;
    CHECK(ta->ops->ptp_accept(ta, main_lsn, pmac, &pport) == NK_NET_AGAIN,
          "slot-wait: ptp_accept waits while no free PTP slot");

    /* Free one slot on A; pending connect must now be accepted cleanly. */
    if (nfill > 0) (void)ta->ops->ptp_close(ta, fillers[--nfill]);
    int child = NK_NET_AGAIN;
    int poll = 0;
    for (int i = 0; i < 60 && (child < 0 || poll == 0); i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
        tb->ops->pump(tb);
        if (child < 0) child = ta->ops->ptp_accept(ta, main_lsn, pmac, &pport);
        poll = tb->ops->ptp_connect_poll(tb, cli);
    }
    CHECK(child >= 0 && pport == 0x4009u && poll == 1,
          "slot-wait: incoming PTP connect accepted cleanly after slot freed (child=%d poll=%d)",
          child, poll);

    if (child >= 0) ta->ops->ptp_close(ta, child);
    for (int i = 0; i < nfill; i++) ta->ops->ptp_close(ta, fillers[i]);
    if (main_lsn >= 0) ta->ops->ptp_close(ta, main_lsn);
    if (cli >= 0) tb->ops->ptp_close(tb, cli);
    ta->ops->logout(ta);
    tb->ops->logout(tb);
    nk_relay_client_destroy(ca);
    nk_relay_client_destroy(cb);
    srv_destroy(&srv);
}

/* Audit 2026-11-17 (F-33b0dd4e): a lobby burst larger than the event queue is not decoded and
 * dropped. The client leaves the frames it has no room for in its receive buffer (TCP holds the
 * rest), and take_events decodes them as the queue drains, so a scan answer of 70 groups still
 * ends with its SCAN_COMPLETE and events_dropped stays 0. */
static void test_lobby_burst_waits_for_queue_room(void) {
    uint16_t lp = 0u, dp = 0u;
    TSock ls = t_listen_loopback(&lp);
    TSock ds = t_listen_loopback(&dp);
    NkAdhocRelayConfig cfg = {"127.0.0.1", lp, dp, NK_RELAY_PORT_OFFSET};
    NkAdhocRelayClient *c = nk_relay_client_create(&cfg);
    CHECK(c != NULL, "lobby burst: client created");
    NkAdhocTransport *t = nk_relay_client_transport(c);
    static const uint8_t mac[6] = {0x02, 1, 2, 3, 4, 6};
    CHECK(t->ops->login(t, mac, "Burst", "ULUS10000") == NK_NET_OK, "lobby burst: login queued");
    TSock accepted = T_INV;
    for (int i = 0; i < 40 && accepted == T_INV; i++) {
        accepted = accept(ls, NULL, NULL);
        t->ops->pump(t);
    }
    CHECK(accepted != T_INV, "lobby burst: server accepted the lobby client");
    enum { GROUPS = 70 };
    uint8_t burst[GROUPS * NK_LOBBY_S2C_SCAN_SIZE + 1u];
    for (unsigned g = 0u; g < GROUPS; g++) {
        uint8_t *f = burst + g * NK_LOBBY_S2C_SCAN_SIZE;
        memset(f, 0, NK_LOBBY_S2C_SCAN_SIZE);
        f[0] = (uint8_t)NK_LOBBY_SCAN;
        memcpy(f + 1, "GRP", 3u);
        f[4] = (uint8_t)('0' + g / 10u);
        f[5] = (uint8_t)('0' + g % 10u);
        f[9] = 0x02u;
        f[14] = (uint8_t)g;
    }
    burst[sizeof(burst) - 1u] = (uint8_t)NK_LOBBY_SCAN_COMPLETE;
    CHECK(t_send_all(accepted, burst, sizeof(burst)), "lobby burst: server sent 70 groups and the end");
    for (int i = 0; i < 40; i++) {
        t->ops->pump(t);
        test_sleep_ms(1u);
    }
    unsigned scans = 0u, completes = 0u;
    NkLobbyEvent ev[16];
    for (int round = 0; round < 40; round++) {
        const size_t n = t->ops->take_events(t, ev, 16u);
        for (size_t i = 0u; i < n; i++) {
            if (ev[i].kind == NK_LOBBY_SCAN) scans++;
            if (ev[i].kind == NK_LOBBY_SCAN_COMPLETE) completes++;
        }
        if (completes) break;
    }
    CHECK(scans == GROUPS && completes == 1u && t->ops->events_dropped(t) == 0u,
          "lobby burst: every group and the SCAN_COMPLETE arrive (scans %u, completes %u, dropped %u)",
          scans, completes, (unsigned)t->ops->events_dropped(t));
    t_close(accepted);
    t_close(ls);
    t_close(ds);
    nk_relay_client_destroy(c);
}

/* Audit 2026-11-17 (F-008632bb, F-4e51f13d): the relay client refuses a local PTP port by the HLE
 * layer's rule (hle_network.c adhoc_ptp_port_in_use, after PPSSPP's isPTPPortInUse): a listen only
 * for another listener on the port, an open only for a stream with the same local port, peer MAC
 * and peer port. A listener, its accepted children and streams to other peers share the port. */
static void test_ptp_port_reuse(void) {
    TestServer srv;
    srv_init(&srv);
    NkAdhocRelayConfig cfg = {"127.0.0.1", srv.lobby_port, srv.data_port, NK_RELAY_PORT_OFFSET};
    NkAdhocRelayClient *ca = nk_relay_client_create(&cfg);
    NkAdhocRelayClient *cb = nk_relay_client_create(&cfg);
    CHECK(ca && cb, "port reuse: clients created");
    NkAdhocTransport *ta = nk_relay_client_transport(ca);
    NkAdhocTransport *tb = nk_relay_client_transport(cb);
    static const uint8_t mac_a[6] = {0x02, 0xaa, 0xbb, 0x02, 0x00, 0x01};
    static const uint8_t mac_b[6] = {0x02, 0xaa, 0xbb, 0x02, 0x00, 0x02};
    static const uint8_t mac_c[6] = {0x02, 0xaa, 0xbb, 0x02, 0x00, 0x03};
    static const uint8_t grp[8] = {'P', 'O', 'R', 'T', 'T', 'E', 'S', 'T'};
    CHECK(ta->ops->login(ta, mac_a, "Alpha", "ULUS10001") == NK_NET_OK &&
          tb->ops->login(tb, mac_b, "Beta", "ULUS10001") == NK_NET_OK,
          "port reuse: both logged in");
    for (int i = 0; i < 30; i++) { srv_pump(&srv); ta->ops->pump(ta); tb->ops->pump(tb); }
    CHECK(ta->ops->connect(ta, grp) == NK_NET_OK && tb->ops->connect(tb, grp) == NK_NET_OK,
          "port reuse: joined group");
    for (int i = 0; i < 30; i++) { srv_pump(&srv); ta->ops->pump(ta); tb->ops->pump(tb); }

    /* A listens on 0x4001, then opens streams from 0x4001 to two different peers. */
    const int lsn = ta->ops->ptp_listen(ta, 0x4001u, 4);
    CHECK(lsn >= 0, "port reuse: A listens on 0x4001");
    const int to_b = ta->ops->ptp_open(ta, 0x4001u, mac_b, 0x4100u);
    CHECK(to_b >= 0, "port reuse: A opens 0x4001 -> B beside its listener on 0x4001 (got %d)", to_b);
    const int to_c = ta->ops->ptp_open(ta, 0x4001u, mac_c, 0x4100u);
    CHECK(to_c >= 0, "port reuse: A opens 0x4001 -> C beside a stream 0x4001 -> B (got %d)", to_c);
    CHECK(ta->ops->ptp_open(ta, 0x4001u, mac_b, 0x4100u) == NK_NET_PORT_IN_USE,
          "port reuse: a second stream 0x4001 -> B:0x4100 is refused");
    CHECK(ta->ops->ptp_listen(ta, 0x4001u, 1) == NK_NET_PORT_IN_USE,
          "port reuse: a second listener on 0x4001 is refused");
    if (to_b >= 0) ta->ops->ptp_close(ta, to_b);
    if (to_c >= 0) ta->ops->ptp_close(ta, to_c);
    for (int i = 0; i < 20; i++) { srv_pump(&srv); ta->ops->pump(ta); tb->ops->pump(tb); }

    /* B connects to A's listener; A accepts, closes the listener and listens again. */
    const int cli = tb->ops->ptp_open(tb, 0x4009u, mac_a, 0x4001u);
    CHECK(cli >= 0, "port reuse: B connects to A:0x4001");
    uint8_t pmac[6] = {0};
    uint16_t pport = 0u;
    int child = NK_NET_AGAIN;
    for (int i = 0; i < 60 && child < 0; i++) {
        srv_pump(&srv);
        ta->ops->pump(ta);
        tb->ops->pump(tb);
        child = ta->ops->ptp_accept(ta, lsn, pmac, &pport);
    }
    CHECK(child >= 0 && pport == 0x4009u, "port reuse: A accepts B on 0x4001 (child=%d)", child);
    ta->ops->ptp_close(ta, lsn);
    const int lsn2 = ta->ops->ptp_listen(ta, 0x4001u, 4);
    CHECK(lsn2 >= 0, "port reuse: A listens on 0x4001 again while the accepted child lives (got %d)",
          lsn2);

    if (lsn2 >= 0) ta->ops->ptp_close(ta, lsn2);
    if (child >= 0) ta->ops->ptp_close(ta, child);
    if (cli >= 0) tb->ops->ptp_close(tb, cli);
    ta->ops->logout(ta);
    tb->ops->logout(tb);
    nk_relay_client_destroy(ca);
    nk_relay_client_destroy(cb);
    srv_destroy(&srv);
}

/* Audit 2026-11-17 (F-45cfed21): a host name that resolves to more than one address is tried in
 * order until the lobby connects. "localhost" resolves through the hosts table, with no network
 * use; Windows lists ::1 before 127.0.0.1, and the test server listens on 127.0.0.1 only, so the
 * first address is refused and the client must go on to the IPv4 one. Where localhost resolves to
 * 127.0.0.1 alone (a stock Ubuntu hosts file) the case passes on its first address. */
static void test_lobby_tries_each_resolved_address(void) {
    TestServer srv;
    srv_init(&srv);
    NkAdhocRelayConfig cfg = {"localhost", srv.lobby_port, srv.data_port, NK_RELAY_PORT_OFFSET};
    NkAdhocRelayClient *c = nk_relay_client_create(&cfg);
    CHECK(c != NULL, "addr fallback: client created");
    NkAdhocTransport *t = nk_relay_client_transport(c);
    static const uint8_t mac[6] = {0x02, 0x31, 0x32, 0x33, 0x34, 0x35};
    static const uint8_t grp[8] = {'F', 'A', 'L', 'L', 'B', 'A', 'C', 'K'};
    CHECK(t->ops->login(t, mac, "Fallback", "ULUS10003") == NK_NET_OK,
          "addr fallback: login queued");
    const int pdp = t->ops->pdp_open(t, 0x3100u);
    CHECK(pdp >= 0, "addr fallback: PDP socket queued before the lobby connects");
    const uint64_t deadline = test_now_ms() + 10000u;
    while (test_now_ms() < deadline && !nk_relay_client_lobby_connected(c) &&
           !nk_relay_client_lobby_dead(c)) {
        srv_pump(&srv);
        t->ops->pump(t);
        test_sleep_ms(2u);
    }
    CHECK(nk_relay_client_lobby_connected(c),
          "addr fallback: lobby connected through a later address (dead %d, error '%s')",
          (int)nk_relay_client_lobby_dead(c), nk_relay_client_last_error(c));
    CHECK(t->ops->connect(t, grp) == NK_NET_OK, "addr fallback: group join queued");
    int joined = 0;
    for (uint64_t end = test_now_ms() + 5000u; test_now_ms() < end && !joined;) {
        srv_pump(&srv);
        t->ops->pump(t);
        NkLobbyEvent evs[4];
        const size_t n = t->ops->take_events(t, evs, 4u);
        for (size_t k = 0; k < n; k++) {
            if (evs[k].kind == NK_LOBBY_CONNECT_BSSID) joined = 1;
        }
        test_sleep_ms(2u);
    }
    CHECK(joined, "addr fallback: group joined over the address that connected");
    static const uint8_t peer[6] = {0x02, 0x31, 0x32, 0x33, 0x34, 0x36};
    static const uint8_t payload[3] = {1, 2, 3};
    CHECK(pdp < 0 || t->ops->pdp_send(t, pdp, peer, 0x3100u, payload, 3u) == NK_NET_OK,
          "addr fallback: the PDP socket opened before the lobby connected still sends");
    t->ops->logout(t);
    nk_relay_client_destroy(c);
    srv_destroy(&srv);
}

/* Audit 2026-11-17 (F-23278c14): a descriptor can reach FD_SETSIZE only when the open-file
 * limit allows it, and a stock Linux shell's soft limit is 1024 (macOS: 256). The case raises
 * the soft limit within the hard limit first and restores it after; where even the hard limit
 * is too low it prints SKIP and counts neither a pass nor a failure. */
static void test_posix_high_fd_above_fd_setsize(void) {
#ifndef _WIN32
    struct rlimit saved;
    int raised = 0;
    const rlim_t want = (rlim_t)FD_SETSIZE + 64u;
    if (getrlimit(RLIMIT_NOFILE, &saved) == 0 && saved.rlim_cur != RLIM_INFINITY &&
        saved.rlim_cur < want) {
        struct rlimit more = saved;
        more.rlim_cur = (saved.rlim_max == RLIM_INFINITY || saved.rlim_max >= want) ? want
                                                                                   : saved.rlim_max;
        raised = setrlimit(RLIMIT_NOFILE, &more) == 0;
    }
    int dummy_fds[1200];
    int ndummy = 0;
    int out_of_fds = 0;
    while (ndummy < (int)(sizeof(dummy_fds) / sizeof(dummy_fds[0]))) {
        int d = open("/dev/null", O_RDONLY);
        if (d < 0) {
            out_of_fds = (errno == EMFILE);
            break;
        }
        dummy_fds[ndummy++] = d;
        if (d >= (int)FD_SETSIZE + 32) break;
    }
    int high_ok = (ndummy > 0 && dummy_fds[ndummy - 1] >= (int)FD_SETSIZE + 32);
    if (!high_ok && out_of_fds) {
        printf("SKIP: posix-high-fd: the hard open-file limit keeps descriptors below "
               "FD_SETSIZE + 32 (highest %d)\n",
               ndummy > 0 ? dummy_fds[ndummy - 1] : -1);
    } else {
        CHECK(high_ok, "posix-high-fd: raised next fd past FD_SETSIZE (%d)",
              ndummy > 0 ? dummy_fds[ndummy - 1] : -1);
    }
    if (high_ok) {
        TestServer srv;
        srv_init(&srv);
        NkAdhocRelayConfig cfg = {"127.0.0.1", srv.lobby_port, srv.data_port, NK_RELAY_PORT_OFFSET};
        NkAdhocRelayClient *c = nk_relay_client_create(&cfg);
        CHECK(c != NULL, "posix-high-fd: client created");
        NkAdhocTransport *t = nk_relay_client_transport(c);
        static const uint8_t mac[6] = {0x02, 0x55, 0x66, 0x77, 0x88, 0x99};
        CHECK(t->ops->login(t, mac, "HighFd", "ULUS10002") == NK_NET_OK,
              "posix-high-fd: login queued on fd >= FD_SETSIZE");
        for (int i = 0; i < 40 && !nk_relay_client_lobby_connected(c); i++) {
            srv_pump(&srv);
            t->ops->pump(t);
        }
        CHECK(nk_relay_client_lobby_connected(c),
              "posix-high-fd: client connected cleanly with socket fd >= FD_SETSIZE");
        t->ops->logout(t);
        nk_relay_client_destroy(c);
        srv_destroy(&srv);
    }
    for (int i = 0; i < ndummy; i++) close(dummy_fds[i]);
    if (raised) (void)setrlimit(RLIMIT_NOFILE, &saved);
#endif
}

typedef struct AsyncDnsHookCtx {
    int calls;
    unsigned delay_ms;
    bool succeed;
    char last_host[NK_ADHOC_SERVER_MAX_LEN + 1u];
} AsyncDnsHookCtx;

static bool async_dns_test_hook(const char *host, uint8_t out_ipv4[4], void *ctx) {
    AsyncDnsHookCtx *h = (AsyncDnsHookCtx *)ctx;
    h->calls++;
    snprintf(h->last_host, sizeof(h->last_host), "%s", host ? host : "");
    if (h->delay_ms) test_sleep_ms(h->delay_ms);
    if (!h->succeed) return false;
    out_ipv4[0] = 127u;
    out_ipv4[1] = 0u;
    out_ipv4[2] = 0u;
    out_ipv4[3] = 1u;
    return true;
}

static void test_async_dns_resolve_nonblocking_and_failure(void) {
    TestServer srv;
    srv_init(&srv);

    AsyncDnsHookCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.delay_ms = 120u;
    ctx.succeed = true;
    nk_relay_client_set_resolve_hook_for_test(async_dns_test_hook, &ctx);

    NkAdhocRelayConfig cfg = {"slow.example.invalid", srv.lobby_port, srv.data_port,
                              NK_RELAY_PORT_OFFSET};
    NkAdhocRelayClient *c = nk_relay_client_create(&cfg);
    CHECK(c != NULL, "async-dns: client created");
    NkAdhocTransport *t = nk_relay_client_transport(c);
    static const uint8_t mac[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x51};
    static const uint8_t grp[8] = {'D', '9', 'T', 'E', 'S', 'T', '0', '1'};

    const uint64_t t0 = test_now_ms();
    const int lrc = t->ops->login(t, mac, "DnsSlow", "ULUS10009");
    const uint64_t elapsed = test_now_ms() - t0;
    CHECK(lrc == NK_NET_OK, "async-dns: login returns immediately");
    CHECK(elapsed < 50u, "async-dns: login did not block on 120ms DNS (%llu ms)",
          (unsigned long long)elapsed);
    CHECK(!nk_relay_client_lobby_dead(c), "async-dns: lobby stays alive while resolving");
    CHECK(t->ops->connect(t, grp) == NK_NET_OK,
          "async-dns: connect queued cleanly before resolve finishes");
    const int pdp = t->ops->pdp_open(t, 0x3000u);
    CHECK(pdp >= 0, "async-dns: pdp_open queued cleanly before resolve finishes");

    int joined = 0;
    for (int i = 0; i < 80 && !joined; i++) {
        test_sleep_ms(5u);
        srv_pump(&srv);
        t->ops->pump(t);
        NkLobbyEvent evs[4];
        size_t n = t->ops->take_events(t, evs, 4u);
        for (size_t k = 0; k < n; k++) {
            if (evs[k].kind == NK_LOBBY_CONNECT_BSSID) joined = 1;
        }
    }
    CHECK(joined == 1, "async-dns: resolved and processed queued CONNECT_BSSID");
    CHECK(ctx.calls == 1, "async-dns: single resolution per login (calls=%d)", ctx.calls);
    CHECK(strcmp(ctx.last_host, "slow.example.invalid") == 0, "async-dns: resolved configured host");
    t->ops->logout(t);
    nk_relay_client_destroy(c);

    /* Failure surfaces lobby dead + named error asynchronously. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.delay_ms = 20u;
    ctx.succeed = false;
    c = nk_relay_client_create(&cfg);
    CHECK(c != NULL, "async-dns-fail: client created");
    t = nk_relay_client_transport(c);
    CHECK(t->ops->login(t, mac, "DnsFail", "ULUS10009") == NK_NET_OK,
          "async-dns-fail: login queued cleanly");
    for (int i = 0; i < 60 && !nk_relay_client_lobby_dead(c); i++) {
        test_sleep_ms(5u);
        t->ops->pump(t);
    }
    CHECK(nk_relay_client_lobby_dead(c), "async-dns-fail: dead flag set after failed resolve");
    CHECK(strstr(nk_relay_client_last_error(c), "resolve") != NULL,
          "async-dns-fail: last_error mentions resolve (%s)", nk_relay_client_last_error(c));
    nk_relay_client_destroy(c);
    nk_relay_client_set_resolve_hook_for_test(NULL, NULL);
    srv_destroy(&srv);
}

/* Flags shared between the test thread and the resolver thread. */
#ifdef _WIN32
typedef volatile LONG TFlag;
static int t_flag_get(TFlag *f) { return (int)InterlockedCompareExchange(f, 0, 0); }
static void t_flag_set(TFlag *f, int v) { (void)InterlockedExchange(f, (LONG)v); }
#else
typedef int TFlag;
static pthread_mutex_t g_flag_lock = PTHREAD_MUTEX_INITIALIZER;
static int t_flag_get(TFlag *f) {
    (void)pthread_mutex_lock(&g_flag_lock);
    const int v = *f;
    (void)pthread_mutex_unlock(&g_flag_lock);
    return v;
}
static void t_flag_set(TFlag *f, int v) {
    (void)pthread_mutex_lock(&g_flag_lock);
    *f = v;
    (void)pthread_mutex_unlock(&g_flag_lock);
}
#endif

typedef struct LatchHookCtx {
    TFlag started;
    TFlag release;
    TFlag returned;
    TFlag winsock_ok;
} LatchHookCtx;

/* Static so a detached resolver may still use it after the client is gone. */
static LatchHookCtx g_latch;

/* Stands in for a slow getaddrinfo: blocks until the test releases it (at most 3 s), then uses
 * Winsock the way getaddrinfo would. */
static bool latch_resolve_hook(const char *host, uint8_t out_ipv4[4], void *ctx) {
    LatchHookCtx *h = (LatchHookCtx *)ctx;
    (void)host;
    t_flag_set(&h->started, 1);
    const uint64_t end = test_now_ms() + 3000u;
    while (!t_flag_get(&h->release) && test_now_ms() < end) test_sleep_ms(1u);
#ifdef _WIN32
    SOCKET probe = socket(AF_INET, SOCK_STREAM, 0);
    t_flag_set(&h->winsock_ok, probe != INVALID_SOCKET);
    if (probe != INVALID_SOCKET) closesocket(probe);
#else
    t_flag_set(&h->winsock_ok, 1);
#endif
    out_ipv4[0] = 127u;
    out_ipv4[1] = 0u;
    out_ipv4[2] = 0u;
    out_ipv4[3] = 1u;
    t_flag_set(&h->returned, 1);
    return true;
}

/* Audit 2026-11-17 (F-b9935c48, F-5a546d12): destroying a client while its server name is still
 * resolving does not wait for the lookup. The resolver runs on detached, frees its job itself
 * once it finishes, and keeps its own Winsock reference, so the lookup still works after the
 * client's WSACleanup dropped the last other one. */
static void test_destroy_detaches_pending_resolve(void) {
    memset((void *)&g_latch, 0, sizeof(g_latch));
#ifdef _WIN32
    /* Drop main's reference: from here only the client and its resolver hold Winsock. */
    WSACleanup();
#endif
    nk_relay_client_set_resolve_hook_for_test(latch_resolve_hook, &g_latch);
    NkAdhocRelayConfig cfg = {"pending.example.invalid", 1u, 2u, NK_RELAY_PORT_OFFSET};
    NkAdhocRelayClient *c = nk_relay_client_create(&cfg);
    CHECK(c != NULL, "detach: client created");
    const unsigned live_before = nk_relay_client_resolve_jobs_live_for_test();
    if (c) {
        NkAdhocTransport *t = nk_relay_client_transport(c);
        static const uint8_t mac[6] = {0x02, 0x41, 0x42, 0x43, 0x44, 0x45};
        CHECK(t->ops->login(t, mac, "Detach", "ULUS10009") == NK_NET_OK,
              "detach: login started a lookup");
        for (uint64_t end = test_now_ms() + 2000u;
             test_now_ms() < end && !t_flag_get(&g_latch.started);)
            test_sleep_ms(1u);
        CHECK(t_flag_get(&g_latch.started), "detach: the resolver is inside its lookup");
        nk_relay_client_destroy(c);
        CHECK(!t_flag_get(&g_latch.returned),
              "detach: destroy returned without waiting for the pending lookup");
        CHECK(nk_relay_client_resolve_jobs_live_for_test() == live_before + 1u,
              "detach: the detached resolver still owns its job (live %u, before %u)",
              nk_relay_client_resolve_jobs_live_for_test(), live_before);
    }
    t_flag_set(&g_latch.release, 1);
    for (uint64_t end = test_now_ms() + 5000u;
         test_now_ms() < end && nk_relay_client_resolve_jobs_live_for_test() != live_before;)
        test_sleep_ms(1u);
    CHECK(nk_relay_client_resolve_jobs_live_for_test() == live_before,
          "detach: the resolver freed its job once the lookup finished (live %u)",
          nk_relay_client_resolve_jobs_live_for_test());
    CHECK(t_flag_get(&g_latch.winsock_ok),
          "detach: the lookup still had Winsock after the client's cleanup");
    nk_relay_client_set_resolve_hook_for_test(NULL, NULL);
#ifdef _WIN32
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "detach: Winsock started again for main");
#endif
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
    test_ptp_accept_waits_for_free_slot();
    test_lobby_burst_waits_for_queue_room();
    test_ptp_port_reuse();
    test_lobby_tries_each_resolved_address();
    test_posix_high_fd_above_fd_setsize();
    test_async_dns_resolve_nonblocking_and_failure();
    test_destroy_detaches_pending_resolve();
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
