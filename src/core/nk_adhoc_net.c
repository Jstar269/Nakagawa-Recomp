/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2014- PPSSPP Project
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * The in-process ad hoc medium (nk_adhoc_net.h): no host socket, no network API.
 *
 * The lobby (lo_login, lo_logout, lo_connect, lo_disconnect, lo_scan and the helpers between
 * here and lo_take_events) is translated from PPSSPP's ad hoc server
 * (https://github.com/hrydgard/ppsspp) Core/HLE/proAdhocServer.cpp at revision
 * 3bc60270eae3069f13dfbfdb7292a7e1375de6f6 (GPL-2.0-or-later; that file is PPSSPP's port of
 * Coldbird's aemu server): login_user_data, logout_user, connect_user, disconnect_user and
 * send_scan_results, with the opcode rules of server_loop. Label: derived-translated
 * (docs/provenance/INDEPENDENCE_MODEL.md). This file is conveyed as GPL-3.0-or-later.
 *
 * Modified for this project (2026-11-12; see src/core/PROVENANCE.md):
 *   - C99 over fixed arrays instead of linked lists of user, game and group nodes. A group's
 *     members are an array kept newest first, as PPSSPP links each joiner at the head of the
 *     group's player list; groups carry a creation number, and a scan lists them newest first,
 *     as PPSSPP links each new group at the head of its game's group list.
 *   - A packet the server sends is an event queued on the receiving console. PPSSPP logs a send
 *     that fails and carries on; the medium likewise drops an event its receiver's full queue
 *     cannot hold, and counts it in events_dropped.
 *   - The peer id in CONNECT and DISCONNECT is the console's index plus one, where PPSSPP sends
 *     the client's IP address.
 *   - A group's BSSID is a locally administered address of its own, kept when its creator leaves
 *     and new for a group created again, as in PPSSPP. PPSSPP draws its bytes at random; the
 *     medium numbers them (02:00 then the medium's group count, big-endian), so every group it
 *     creates has its own and a test run is reproducible.
 *   - A MAC already logged in is refused (NK_NET_BAD_ARG). PPSSPP logs it and accepts it; the
 *     medium delivers datagrams by MAC, and two consoles with one MAC would make that ambiguous.
 *   - The login checks are nk_relay_product_ok, nk_relay_mac_ok and nk_relay_nickname_ok
 *     (nk_adhoc_relay.c), PPSSPP's three checks in its order. They take C strings, so a product
 *     code must end after nine bytes and a nickname within 128; a refused login answers
 *     NK_NET_BAD_ARG and leaves the console logged out. A login empties the event queue and sets
 *     events_dropped and pdp_dropped to zero, as a new connection starts with nothing sent and
 *     nothing lost (the sockets the datagram count belongs to closed at the logout before).
 *   - Where PPSSPP logs a client out for a request (a login while logged in, a join while in a
 *     group, a group name with a character outside A-Z, a-z and 0-9, a leave outside a group, a
 *     scan inside one), the medium
 *     logs the console out the same way: it leaves its group (the members are told) and its
 *     sockets close. The call answers NK_NET_NOT_LOGGED_IN, and the events it was already sent
 *     stay for it to take, as bytes already sent reach a client before its connection closes.
 *   - Left out: the product-code overrides (game_product_override), chat, the status log, the
 *     login timeout and the server's own sockets.
 *
 * lo_login was first carried over, with small edits, from an earlier version of this file; a
 * review of pull request #2549 found it, and on 2026-11-12 it was deleted and written again from
 * login_user_data.
 *
 * The PDP half (sockets, datagram queues, delivery by MAC and port inside the sender's group,
 * counted drops) and nk_adhoc_drop_report are this project's own: PPSSPP's lobby server has no
 * counterpart. */

#include "nk_adhoc_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Datagram {
    uint8_t mac[NK_RELAY_MAC_LEN];
    uint16_t port;
    uint32_t len;
    uint8_t *data;
} Datagram;

typedef struct Socket {
    int used;
    uint16_t port;
    Datagram queue[NK_NET_PDP_QUEUE];
    unsigned head, count;
} Socket;

typedef struct Group {
    int used;
    uint64_t number; /* creation order: a larger number is a newer group; never wraps */
    char product[NK_RELAY_PRODUCT_LEN + 1u];
    uint8_t name[NK_RELAY_GROUP_LEN];
    uint8_t bssid[NK_RELAY_MAC_LEN];
    unsigned members[NK_LOOPNET_CONSOLES]; /* console indices, newest first */
    unsigned count;
} Group;

#define NK_NET_PTP_SOCKETS 16u
#define NK_NET_PTP_BACKLOG 8u
#define NK_NET_PTP_STREAM_CAP (128u * 1024u)

typedef struct PtpSocket {
    int used;
    int state; /* 0 closed, 1 listen, 2 connected */
    uint16_t lport;
    uint16_t pport;
    uint8_t peer_mac[NK_RELAY_MAC_LEN];
    int peer_console; /* -1 if none */
    int peer_handle;  /* -1 if none */
    int max_backlog;
    int pending_handles[NK_NET_PTP_BACKLOG];
    unsigned pending_count;
    int peer_closed;
    uint8_t *rx;
    uint32_t rx_len;
    uint32_t rx_cap;
} PtpSocket;

typedef struct Console {
    NkAdhocTransport base; /* first, so the caller's endpoint is the console */
    NkAdhocLoopNet *net;
    unsigned index;
    int logged_in;
    uint8_t mac[NK_RELAY_MAC_LEN];
    char nickname[NK_RELAY_NICKNAME_LEN + 1u]; /* up to 128 bytes, then a NUL */
    char product[NK_RELAY_PRODUCT_LEN + 1u];
    int group; /* index into net->groups, or -1 */
    NkLobbyEvent events[NK_NET_EVENT_QUEUE];
    unsigned ev_head, ev_count;
    uint32_t ev_dropped;
    Socket sockets[NK_NET_PDP_SOCKETS];
    PtpSocket ptp_sockets[NK_NET_PTP_SOCKETS];
    uint16_t next_ptp_port;
    uint32_t pdp_dropped;
} Console;

struct NkAdhocLoopNet {
    Console consoles[NK_LOOPNET_CONSOLES];
    Group groups[NK_LOOPNET_CONSOLES]; /* each console is in one group at most */
    uint64_t groups_made;
    NkLoopTrace trace[NK_LOOPNET_TRACE];
    size_t trace_count; /* events queued since the last nk_loopnet_trace, kept or not */
};

static Console *as_console(NkAdhocTransport *t) { return (Console *)t; }
static const Console *as_const_console(const NkAdhocTransport *t) { return (const Console *)t; }

static uint32_t peer_id(const Console *c) { return c->index + 1u; }

static void free_queue(Socket *s) {
    for (unsigned i = 0; i < s->count; i++) {
        Datagram *d = &s->queue[(s->head + i) % NK_NET_PDP_QUEUE];
        free(d->data);
        d->data = NULL;
    }
    s->head = s->count = 0;
}

static void ptp_free_slot(Console *c, int handle) {
    if (handle < 0 || (unsigned)handle >= NK_NET_PTP_SOCKETS) return;
    PtpSocket *ps = &c->ptp_sockets[handle];
    if (!ps->used) return;
    if (ps->state == 1) {
        for (unsigned i = 0; i < ps->pending_count; i++) {
            ptp_free_slot(c, ps->pending_handles[i]);
        }
    }
    if (ps->peer_console >= 0 && (unsigned)ps->peer_console < NK_LOOPNET_CONSOLES &&
        ps->peer_handle >= 0 && (unsigned)ps->peer_handle < NK_NET_PTP_SOCKETS) {
        PtpSocket *peer = &c->net->consoles[ps->peer_console].ptp_sockets[ps->peer_handle];
        if (peer->used && peer->peer_console == (int)c->index && peer->peer_handle == handle) {
            peer->peer_closed = 1;
            peer->peer_console = -1;
            peer->peer_handle = -1;
        }
    }
    free(ps->rx);
    memset(ps, 0, sizeof *ps);
    ps->peer_console = -1;
    ps->peer_handle = -1;
}

static void close_sockets(Console *c) {
    for (unsigned k = 0; k < NK_NET_PDP_SOCKETS; k++) {
        free_queue(&c->sockets[k]);
        c->sockets[k].used = 0;
    }
    for (unsigned k = 0; k < NK_NET_PTP_SOCKETS; k++) {
        ptp_free_slot(c, (int)k);
    }
}

/* One server packet to one console (PPSSPP: send() on the client's stream). */
static void send_event(Console *to, const NkLobbyEvent *ev) {
    NkAdhocLoopNet *net = to->net;
    if (to->ev_count == NK_NET_EVENT_QUEUE) {
        to->ev_dropped++;
        return;
    }
    to->events[(to->ev_head + to->ev_count) % NK_NET_EVENT_QUEUE] = *ev;
    to->ev_count++;
    if (net->trace_count < NK_LOOPNET_TRACE) {
        NkLoopTrace *tr = &net->trace[net->trace_count];
        tr->console = to->index;
        tr->kind = ev->kind;
        tr->peer_id = ev->peer_id;
    }
    net->trace_count++;
}

/* OPCODE_CONNECT: tells `to` that `who` is in its group. */
static void send_connect(Console *to, const Console *who) {
    NkLobbyEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = NK_LOBBY_CONNECT;
    memcpy(ev.nickname, who->nickname, sizeof ev.nickname);
    memcpy(ev.mac, who->mac, NK_RELAY_MAC_LEN);
    ev.peer_id = peer_id(who);
    send_event(to, &ev);
}

/* PPSSPP disconnect_user, for a console in a group: it leaves the member list, each remaining
 * member (newest first) hears OPCODE_DISCONNECT with its id, and an empty group is deleted. The
 * leaver hears nothing. */
static void leave_group(Console *c) {
    NkAdhocLoopNet *net = c->net;
    Group *g = &net->groups[c->group];
    unsigned at = 0;
    while (at < g->count && g->members[at] != c->index) at++;
    if (at < g->count) {
        for (unsigned i = at; i + 1u < g->count; i++) g->members[i] = g->members[i + 1u];
        g->count--;
    }
    for (unsigned i = 0; i < g->count; i++) {
        NkLobbyEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = NK_LOBBY_DISCONNECT;
        ev.peer_id = peer_id(c);
        send_event(&net->consoles[g->members[i]], &ev);
    }
    if (g->count == 0u) memset(g, 0, sizeof *g);
    c->group = -1;
}

/* PPSSPP logout_user: leave the group, then the connection closes. On the medium that closes the
 * console's sockets too. */
static void log_out(Console *c) {
    if (c->group >= 0) leave_group(c);
    close_sockets(c);
    c->logged_in = 0;
}

/* A request PPSSPP's server answers by logging the client out. */
static int drop_client(Console *c) {
    log_out(c);
    return NK_NET_NOT_LOGGED_IN;
}

/* connect_user's group name check: up to the first zero byte, A-Z, a-z and 0-9 only. */
static int group_name_ok(const uint8_t name[NK_RELAY_GROUP_LEN]) {
    for (unsigned i = 0; i < NK_RELAY_GROUP_LEN; i++) {
        const uint8_t ch = name[i];
        if (ch == 0u) break;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) continue;
        return 0;
    }
    return 1;
}

/* A group of this product with this name, compared as PPSSPP does (strncmp, so bytes after a
 * zero byte do not count), or -1. */
static int find_group(const NkAdhocLoopNet *net, const char *product,
                      const uint8_t name[NK_RELAY_GROUP_LEN]) {
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        const Group *g = &net->groups[i];
        if (g->used && strncmp(g->product, product, NK_RELAY_PRODUCT_LEN) == 0 &&
            strncmp((const char *)g->name, (const char *)name, NK_RELAY_GROUP_LEN) == 0)
            return (int)i;
    }
    return -1;
}

static int bssid_in_use(const NkAdhocLoopNet *net, uint32_t low) {
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        const Group *g = &net->groups[i];
        if (g->used && (uint32_t)g->number == low) return 1;
    }
    return 0;
}

static int make_group(NkAdhocLoopNet *net, const char *product, const uint8_t name[NK_RELAY_GROUP_LEN]) {
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        Group *g = &net->groups[i];
        if (g->used) continue;
        /* The BSSID carries the number's low 32 bits; a number whose BSSID a live group holds
         * (only after 2^32 creations) is skipped, so two live groups never share one. */
        uint64_t number;
        do number = ++net->groups_made;
        while (bssid_in_use(net, (uint32_t)number));
        memset(g, 0, sizeof *g);
        g->used = 1;
        g->number = number;
        memcpy(g->product, product, NK_RELAY_PRODUCT_LEN);
        memcpy(g->name, name, NK_RELAY_GROUP_LEN);
        g->bssid[0] = 0x02u; /* locally administered, unicast */
        g->bssid[1] = 0x00u;
        g->bssid[2] = (uint8_t)(g->number >> 24);
        g->bssid[3] = (uint8_t)(g->number >> 16);
        g->bssid[4] = (uint8_t)(g->number >> 8);
        g->bssid[5] = (uint8_t)g->number;
        return (int)i;
    }
    return -1;
}

/* login_user_data. The server checks the product code (nine characters A-Z or 0-9), then the
 * MAC (neither all ones nor all zero), then the nickname (a first byte that is not zero), and
 * refuses a login that fails any of them: PPSSPP logs the client out, which for a client not yet
 * logged in ends its connection; the console stays logged out and the call answers
 * NK_NET_BAD_ARG. A login that passes saves the MAC, the nickname and the product code, which
 * stands for PPSSPP's game node: groups are found per product code (find_group).
 * The medium's own rules: a MAC another console holds is refused (PPSSPP logs it and accepts it),
 * and a login starts with an empty event queue and both drop counts at zero, as a new connection
 * does. A LOGIN from a console already logged in is an opcode PPSSPP's server_loop does not take
 * in that state, so the server logs the console out. */
static int lo_login(NkAdhocTransport *t, const uint8_t mac[NK_RELAY_MAC_LEN], const char *nickname,
                    const char *product) {
    Console *c = as_console(t);
    if (c->logged_in) return drop_client(c);
    if (!nk_relay_product_ok(product) || !nk_relay_mac_ok(mac) || !nk_relay_nickname_ok(nickname))
        return NK_NET_BAD_ARG;
    const NkAdhocLoopNet *net = c->net;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        const Console *other = &net->consoles[i];
        if (other->logged_in && memcmp(other->mac, mac, NK_RELAY_MAC_LEN) == 0) return NK_NET_BAD_ARG;
    }
    memcpy(c->mac, mac, NK_RELAY_MAC_LEN);
    memset(c->nickname, 0, sizeof c->nickname);
    memcpy(c->nickname, nickname, strlen(nickname)); /* at most NK_RELAY_NICKNAME_LEN, checked */
    memcpy(c->product, product, NK_RELAY_PRODUCT_LEN);
    c->product[NK_RELAY_PRODUCT_LEN] = '\0';
    c->group = -1;
    c->ev_head = c->ev_count = 0u;
    c->ev_dropped = 0u;
    c->pdp_dropped = 0u;
    c->logged_in = 1;
    return NK_NET_OK;
}

/* The client ends its connection: the server runs logout_user, and what was not yet read goes
 * with the connection. */
static int lo_logout(NkAdhocTransport *t) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    log_out(c);
    c->ev_head = c->ev_count = 0;
    return NK_NET_OK;
}

/* connect_user. For each member already in the group, newest first: the member hears of the
 * joiner, then the joiner hears of the member. The joiner then joins the head of the member list
 * and hears the group's BSSID (OPCODE_CONNECT_BSSID). A new group gets a BSSID of its own. */
static int lo_connect(NkAdhocTransport *t, const uint8_t group[NK_RELAY_GROUP_LEN]) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (!group) return NK_NET_BAD_ARG;
    if (!group_name_ok(group) || c->group >= 0) return drop_client(c);
    NkAdhocLoopNet *net = c->net;
    int gi = find_group(net, c->product, group);
    if (gi < 0) gi = make_group(net, c->product, group);
    /* Cannot fail: a console outside every group leaves a free slot. PPSSPP logs the client out
     * when it cannot allocate a group. */
    if (gi < 0) return drop_client(c);
    Group *g = &net->groups[gi];
    for (unsigned i = 0; i < g->count; i++) {
        Console *member = &net->consoles[g->members[i]];
        send_connect(member, c);
        send_connect(c, member);
    }
    for (unsigned i = g->count; i > 0u; i--) g->members[i] = g->members[i - 1u];
    g->members[0] = c->index;
    g->count++;
    c->group = gi;
    NkLobbyEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = NK_LOBBY_CONNECT_BSSID;
    memcpy(ev.bssid, g->bssid, NK_RELAY_MAC_LEN);
    send_event(c, &ev);
    return NK_NET_OK;
}

static int lo_disconnect(NkAdhocTransport *t) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (c->group < 0) return drop_client(c);
    leave_group(c);
    return NK_NET_OK;
}

/* send_scan_results: for a console outside every group, one OPCODE_SCAN per group of its product,
 * newest group first, each with the group's BSSID, then OPCODE_SCAN_COMPLETE. */
static int lo_scan(NkAdhocTransport *t) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (c->group >= 0) return drop_client(c);
    NkAdhocLoopNet *net = c->net;
    uint64_t below = UINT64_MAX;
    for (;;) {
        int next = -1;
        for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
            const Group *g = &net->groups[i];
            if (!g->used || g->number >= below || strncmp(g->product, c->product, NK_RELAY_PRODUCT_LEN) != 0)
                continue;
            if (next < 0 || g->number > net->groups[next].number) next = (int)i;
        }
        if (next < 0) break;
        const Group *g = &net->groups[next];
        NkLobbyEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = NK_LOBBY_SCAN;
        memcpy(ev.group, g->name, NK_RELAY_GROUP_LEN);
        memcpy(ev.bssid, g->bssid, NK_RELAY_MAC_LEN);
        send_event(c, &ev);
        below = g->number;
    }
    NkLobbyEvent done;
    memset(&done, 0, sizeof done);
    done.kind = NK_LOBBY_SCAN_COMPLETE;
    send_event(c, &done);
    return NK_NET_OK;
}

static size_t lo_take_events(NkAdhocTransport *t, NkLobbyEvent *out, size_t max) {
    Console *c = as_console(t);
    size_t n = 0;
    while (out && n < max && c->ev_count) {
        out[n++] = c->events[c->ev_head];
        c->ev_head = (c->ev_head + 1u) % NK_NET_EVENT_QUEUE;
        c->ev_count--;
    }
    return n;
}

static uint32_t lo_events_dropped(const NkAdhocTransport *t) { return as_const_console(t)->ev_dropped; }

static Socket *socket_at(Console *c, int handle) {
    if (handle < 0 || (unsigned)handle >= NK_NET_PDP_SOCKETS || !c->sockets[handle].used) return NULL;
    return &c->sockets[handle];
}

static int lo_pdp_open(NkAdhocTransport *t, uint16_t port) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (port == 0u) return NK_NET_BAD_ARG;
    int slot = -1;
    for (unsigned i = 0; i < NK_NET_PDP_SOCKETS; i++) {
        if (c->sockets[i].used && c->sockets[i].port == port) return NK_NET_PORT_IN_USE;
        if (!c->sockets[i].used && slot < 0) slot = (int)i;
    }
    if (slot < 0) return NK_NET_NO_SLOT;
    Socket *s = &c->sockets[slot];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->port = port;
    return slot;
}

static int lo_pdp_send(NkAdhocTransport *t, int handle, const uint8_t mac[NK_RELAY_MAC_LEN],
                       uint16_t port, const void *data, uint32_t len) {
    Console *c = as_console(t);
    Socket *from = socket_at(c, handle);
    if (!from) return NK_NET_NO_SOCKET;
    if (!mac || (mac[0] & 1u) || port == 0u || (len && !data)) return NK_NET_BAD_ARG;
    if (len > NK_RELAY_PDP_MAX) return NK_NET_TOO_BIG;
    if (c->group < 0) return NK_NET_NOT_IN_GROUP;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        Console *to = &c->net->consoles[i];
        if (!to->logged_in || to->group != c->group || memcmp(to->mac, mac, NK_RELAY_MAC_LEN)) continue;
        for (unsigned k = 0; k < NK_NET_PDP_SOCKETS; k++) {
            Socket *s = &to->sockets[k];
            if (!s->used || s->port != port) continue;
            uint8_t *copy = len ? malloc(len) : NULL;
            if (s->count == NK_NET_PDP_QUEUE || (len && !copy)) {
                free(copy);
                to->pdp_dropped++;
                return NK_NET_OK;
            }
            if (len) memcpy(copy, data, len);
            Datagram *d = &s->queue[(s->head + s->count) % NK_NET_PDP_QUEUE];
            memcpy(d->mac, c->mac, NK_RELAY_MAC_LEN);
            d->port = from->port;
            d->len = len;
            d->data = copy;
            s->count++;
            return NK_NET_OK;
        }
    }
    return NK_NET_OK; /* nobody holds that address in the group: dropped, as UDP drops it */
}

static int lo_pdp_recv(NkAdhocTransport *t, int handle, uint8_t mac[NK_RELAY_MAC_LEN], uint16_t *port,
                       void *buf, uint32_t cap, uint32_t *len) {
    Socket *s = socket_at(as_console(t), handle);
    if (!s) return NK_NET_NO_SOCKET;
    if (!mac || !port || !len || (cap && !buf)) return NK_NET_BAD_ARG;
    if (!s->count) return NK_NET_AGAIN;
    Datagram *d = &s->queue[s->head];
    *len = d->len;
    if (d->len > cap) return NK_NET_SHORT_BUFFER;
    memcpy(mac, d->mac, NK_RELAY_MAC_LEN);
    *port = d->port;
    if (d->len) memcpy(buf, d->data, d->len);
    free(d->data);
    d->data = NULL;
    s->head = (s->head + 1u) % NK_NET_PDP_QUEUE;
    s->count--;
    return NK_NET_OK;
}

static int lo_pdp_close(NkAdhocTransport *t, int handle) {
    Socket *s = socket_at(as_console(t), handle);
    if (!s) return NK_NET_NO_SOCKET;
    free_queue(s);
    s->used = 0;
    return NK_NET_OK;
}

static uint32_t lo_pdp_dropped(const NkAdhocTransport *t) { return as_const_console(t)->pdp_dropped; }

static void lo_pump(NkAdhocTransport *t) { (void)t; }

static uint32_t lo_pdp_peek(NkAdhocTransport *t, int handle) {
    const Socket *s = socket_at(as_console(t), handle);
    if (!s || !s->count) return 0u;
    return s->queue[s->head].len;
}

static PtpSocket *ptp_at(Console *c, int handle) {
    if (handle < 0 || (unsigned)handle >= NK_NET_PTP_SOCKETS || !c->ptp_sockets[handle].used)
        return NULL;
    return &c->ptp_sockets[handle];
}

static int ptp_alloc_slot(Console *c) {
    for (unsigned i = 0; i < NK_NET_PTP_SOCKETS; i++) {
        if (!c->ptp_sockets[i].used) {
            PtpSocket *ps = &c->ptp_sockets[i];
            memset(ps, 0, sizeof *ps);
            ps->used = 1;
            ps->peer_console = -1;
            ps->peer_handle = -1;
            return (int)i;
        }
    }
    return NK_NET_NO_SLOT;
}

/* Any socket on the port: an automatic port skips it. */
static int ptp_port_in_use(const Console *c, uint16_t port) {
    for (unsigned i = 0; i < NK_NET_PTP_SOCKETS; i++) {
        if (c->ptp_sockets[i].used && c->ptp_sockets[i].lport == port) return 1;
    }
    return 0;
}

/* The ports a listen or an open the caller named is refused on, by the HLE layer's rule
 * (src/rt/hle_network.c adhoc_ptp_port_in_use, after PPSSPP's isPTPPortInUse): a listen when
 * another listener holds the port, an open when a stream already joins the same local port, peer
 * MAC and peer port. A listener, its accepted children and streams to other peers share a port. */
static int ptp_listen_port_taken(const Console *c, uint16_t port) {
    for (unsigned i = 0; i < NK_NET_PTP_SOCKETS; i++) {
        const PtpSocket *ps = &c->ptp_sockets[i];
        if (ps->used && ps->state == 1 && ps->lport == port) return 1;
    }
    return 0;
}

static int ptp_stream_taken(const Console *c, uint16_t port, const uint8_t dst[NK_RELAY_MAC_LEN],
                            uint16_t dport) {
    for (unsigned i = 0; i < NK_NET_PTP_SOCKETS; i++) {
        const PtpSocket *ps = &c->ptp_sockets[i];
        if (ps->used && ps->state != 1 && ps->lport == port && ps->pport == dport &&
            memcmp(ps->peer_mac, dst, NK_RELAY_MAC_LEN) == 0)
            return 1;
    }
    return 0;
}

static uint16_t ptp_pick_port(Console *c) {
    if (c->next_ptp_port < 0x4000u) c->next_ptp_port = 0x4000u;
    for (unsigned n = 0; n < 16384u; n++) {
        const uint16_t candidate = c->next_ptp_port++;
        if (c->next_ptp_port < 0x4000u) c->next_ptp_port = 0x4000u;
        if (candidate != 0u && candidate != 0xffffu && !ptp_port_in_use(c, candidate))
            return candidate;
    }
    return 0u;
}

static int lo_ptp_listen(NkAdhocTransport *t, uint16_t sport, int backlog) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (sport == 0u) sport = ptp_pick_port(c);
    if (sport == 0u) return NK_NET_NO_SLOT;
    if (ptp_listen_port_taken(c, sport)) return NK_NET_PORT_IN_USE;
    const int slot = ptp_alloc_slot(c);
    if (slot < 0) return slot;
    PtpSocket *ps = &c->ptp_sockets[slot];
    ps->state = 1;
    ps->lport = sport;
    ps->max_backlog = backlog < 1 ? 1 : (backlog > (int)NK_NET_PTP_BACKLOG ? (int)NK_NET_PTP_BACKLOG : backlog);
    return slot;
}

static int lo_ptp_accept(NkAdhocTransport *t, int listen_handle, uint8_t peer_mac[NK_RELAY_MAC_LEN],
                         uint16_t *peer_port) {
    Console *c = as_console(t);
    PtpSocket *ls = ptp_at(c, listen_handle);
    if (!ls || ls->state != 1) return NK_NET_NO_SOCKET;
    if (ls->pending_count == 0u) return NK_NET_AGAIN;
    const int child_handle = ls->pending_handles[0];
    for (unsigned i = 1u; i < ls->pending_count; i++) {
        ls->pending_handles[i - 1u] = ls->pending_handles[i];
    }
    ls->pending_count--;
    PtpSocket *child = ptp_at(c, child_handle);
    if (!child) return NK_NET_NO_SOCKET;
    if (peer_mac) memcpy(peer_mac, child->peer_mac, NK_RELAY_MAC_LEN);
    if (peer_port) *peer_port = child->pport;
    return child_handle;
}

static int lo_ptp_open(NkAdhocTransport *t, uint16_t sport, const uint8_t dst[NK_RELAY_MAC_LEN],
                       uint16_t dport) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (!dst || dport == 0u) return NK_NET_BAD_ARG;
    if (sport == 0u) sport = ptp_pick_port(c);
    if (sport == 0u) return NK_NET_NO_SLOT;
    if (ptp_stream_taken(c, sport, dst, dport)) return NK_NET_PORT_IN_USE;
    if (c->group < 0) return NK_NET_CONNECTION_REFUSED;

    Console *target = NULL;
    PtpSocket *listener = NULL;
    int listen_idx = -1;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        Console *peer = &c->net->consoles[i];
        if (!peer->logged_in || peer->group != c->group || memcmp(peer->mac, dst, NK_RELAY_MAC_LEN))
            continue;
        target = peer;
        for (unsigned k = 0; k < NK_NET_PTP_SOCKETS; k++) {
            if (peer->ptp_sockets[k].used && peer->ptp_sockets[k].state == 1 &&
                peer->ptp_sockets[k].lport == dport) {
                listener = &peer->ptp_sockets[k];
                listen_idx = (int)k;
                break;
            }
        }
        break;
    }
    if (!target || !listener || (int)listener->pending_count >= listener->max_backlog)
        return NK_NET_CONNECTION_REFUSED;
    (void)listen_idx;
    const int cli_slot = ptp_alloc_slot(c);
    if (cli_slot < 0) return cli_slot;
    const int srv_slot = ptp_alloc_slot(target);
    if (srv_slot < 0) {
        ptp_free_slot(c, cli_slot);
        return NK_NET_CONNECTION_REFUSED;
    }

    PtpSocket *cli = &c->ptp_sockets[cli_slot];
    PtpSocket *srv = &target->ptp_sockets[srv_slot];
    cli->state = 2;
    cli->lport = sport;
    cli->pport = dport;
    memcpy(cli->peer_mac, target->mac, NK_RELAY_MAC_LEN);
    cli->peer_console = (int)target->index;
    cli->peer_handle = srv_slot;

    srv->state = 2;
    srv->lport = dport;
    srv->pport = sport;
    memcpy(srv->peer_mac, c->mac, NK_RELAY_MAC_LEN);
    srv->peer_console = (int)c->index;
    srv->peer_handle = cli_slot;
    listener->pending_handles[listener->pending_count++] = srv_slot;
    return cli_slot;
}

static int lo_ptp_connect_poll(NkAdhocTransport *t, int handle) {
    Console *c = as_console(t);
    const PtpSocket *ps = ptp_at(c, handle);
    if (!ps) return NK_NET_NO_SOCKET;
    if (ps->state == 2) return 1;
    return NK_NET_CONNECTION_REFUSED;
}

static int lo_ptp_send(NkAdhocTransport *t, int handle, const void *data, uint32_t len,
                       uint32_t *sent) {
    Console *c = as_console(t);
    if (sent) *sent = 0u;
    PtpSocket *ps = ptp_at(c, handle);
    if (!ps || ps->state != 2) return NK_NET_NO_SOCKET;
    if (ps->peer_closed || ps->peer_console < 0 || ps->peer_handle < 0) return NK_NET_DEAD;
    if (len == 0u) return NK_NET_OK;
    if (!data) return NK_NET_BAD_ARG;
    Console *pc = &c->net->consoles[ps->peer_console];
    PtpSocket *peer = ptp_at(pc, ps->peer_handle);
    if (!peer || peer->state != 2) return NK_NET_DEAD;
    const uint32_t room = peer->rx_len < NK_NET_PTP_STREAM_CAP ? NK_NET_PTP_STREAM_CAP - peer->rx_len : 0u;
    if (room == 0u) return NK_NET_AGAIN;
    const uint32_t take = len < room ? len : room;
    if (peer->rx_len + take > peer->rx_cap) {
        uint32_t new_cap = peer->rx_cap ? peer->rx_cap * 2u : 4096u;
        while (new_cap < peer->rx_len + take) new_cap *= 2u;
        if (new_cap > NK_NET_PTP_STREAM_CAP) new_cap = NK_NET_PTP_STREAM_CAP;
        uint8_t *grown = realloc(peer->rx, new_cap);
        if (!grown) return NK_NET_AGAIN;
        peer->rx = grown;
        peer->rx_cap = new_cap;
    }
    memcpy(peer->rx + peer->rx_len, data, take);
    peer->rx_len += take;
    if (sent) *sent = take;
    return NK_NET_OK;
}

static int lo_ptp_recv(NkAdhocTransport *t, int handle, void *buf, uint32_t cap, uint32_t *len) {
    Console *c = as_console(t);
    if (len) *len = 0u;
    PtpSocket *ps = ptp_at(c, handle);
    if (!ps || ps->state != 2) return NK_NET_NO_SOCKET;
    if (!len || (cap && !buf)) return NK_NET_BAD_ARG;
    if (ps->rx_len == 0u) return ps->peer_closed ? NK_NET_DEAD : NK_NET_AGAIN;
    if (cap == 0u) return NK_NET_OK;
    const uint32_t take = ps->rx_len < cap ? ps->rx_len : cap;
    memcpy(buf, ps->rx, take);
    if (take < ps->rx_len) memmove(ps->rx, ps->rx + take, ps->rx_len - take);
    ps->rx_len -= take;
    *len = take;
    return NK_NET_OK;
}

static uint32_t lo_ptp_peek(NkAdhocTransport *t, int handle) {
    const PtpSocket *ps = ptp_at(as_console(t), handle);
    return (ps && ps->state == 2) ? ps->rx_len : 0u;
}

static bool lo_ptp_is_dead(const NkAdhocTransport *t, int handle) {
    const Console *c = as_const_console(t);
    if (handle < 0 || (unsigned)handle >= NK_NET_PTP_SOCKETS || !c->ptp_sockets[handle].used)
        return true;
    return c->ptp_sockets[handle].peer_closed != 0;
}

static bool lo_ptp_accept_pending(const NkAdhocTransport *t, int listen_handle) {
    const Console *c = as_const_console(t);
    if (listen_handle < 0 || (unsigned)listen_handle >= NK_NET_PTP_SOCKETS) return false;
    const PtpSocket *ls = &c->ptp_sockets[listen_handle];
    return ls->used && ls->state == 1 && ls->pending_count > 0u;
}

static int lo_ptp_close(NkAdhocTransport *t, int handle) {
    Console *c = as_console(t);
    if (!ptp_at(c, handle)) return NK_NET_NO_SOCKET;
    ptp_free_slot(c, handle);
    return NK_NET_OK;
}

static const NkAdhocTransportOps s_loop_ops = {
    "loopback",
    lo_pump,
    lo_login, lo_logout, lo_connect, lo_disconnect, lo_scan, lo_take_events, lo_events_dropped,
    lo_pdp_open, lo_pdp_send, lo_pdp_recv, lo_pdp_peek, lo_pdp_close, lo_pdp_dropped,
    lo_ptp_listen, lo_ptp_accept_pending, lo_ptp_accept, lo_ptp_open, lo_ptp_connect_poll,
    lo_ptp_send, lo_ptp_recv, lo_ptp_peek, lo_ptp_is_dead, lo_ptp_close,
};

NkAdhocLoopNet *nk_loopnet_create(void) {
    NkAdhocLoopNet *net = calloc(1, sizeof *net);
    if (!net) return NULL;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        net->consoles[i].base.ops = &s_loop_ops;
        net->consoles[i].net = net;
        net->consoles[i].index = i;
        net->consoles[i].group = -1;
    }
    return net;
}

void nk_loopnet_destroy(NkAdhocLoopNet *net) {
    if (!net) return;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        for (unsigned k = 0; k < NK_NET_PDP_SOCKETS; k++) free_queue(&net->consoles[i].sockets[k]);
        for (unsigned k = 0; k < NK_NET_PTP_SOCKETS; k++) ptp_free_slot(&net->consoles[i], (int)k);
    }
    free(net);
}

NkAdhocTransport *nk_loopnet_console(NkAdhocLoopNet *net, unsigned index) {
    if (!net || index >= NK_LOOPNET_CONSOLES) return NULL;
    return &net->consoles[index].base;
}

void nk_loopnet_set_groups_made(NkAdhocLoopNet *net, uint64_t made) {
    if (net) net->groups_made = made;
}

size_t nk_loopnet_trace(NkAdhocLoopNet *net, NkLoopTrace *out, size_t max) {
    if (!net) return 0;
    const size_t kept = net->trace_count < NK_LOOPNET_TRACE ? net->trace_count : NK_LOOPNET_TRACE;
    for (size_t i = 0; out && i < kept && i < max; i++) out[i] = net->trace[i];
    const size_t queued = net->trace_count;
    net->trace_count = 0;
    return queued;
}

int nk_adhoc_drop_report(const NkAdhocTransport *t, unsigned console, char *buf, size_t cap) {
    if (buf && cap > 0u) buf[0] = '\0';
    if (!t || !t->ops || !t->ops->events_dropped) return 0;
    const uint32_t dropped = t->ops->events_dropped(t);
    if (dropped == 0u) return 0;
    /* snprintf cuts the line to fit and terminates it, and returns the full length. */
    char *const dst = (buf && cap > 0u) ? buf : NULL;
    const int n = snprintf(dst, dst ? cap : 0u,
                           "adhoc %s: console %u dropped %lu lobby event(s) since login; its view of "
                           "the group may be incomplete",
                           t->ops->name ? t->ops->name : "?", console, (unsigned long)dropped);
    if (n < 0) {
        if (dst) dst[0] = '\0';
        return 0;
    }
    return n;
}
