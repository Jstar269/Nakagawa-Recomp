/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The in-process ad hoc medium (nk_adhoc_net.h): no host socket, no network API. */

#include "nk_adhoc_net.h"

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
    char product[NK_RELAY_PRODUCT_LEN + 1u];
    uint8_t name[NK_RELAY_GROUP_LEN];
    uint8_t leader[NK_RELAY_MAC_LEN];
} Group;

typedef struct Console {
    NkAdhocTransport base; /* first, so the caller's endpoint is the console */
    NkAdhocLoopNet *net;
    unsigned index;
    int logged_in;
    uint8_t mac[NK_RELAY_MAC_LEN];
    char nickname[NK_RELAY_NICKNAME_LEN];
    char product[NK_RELAY_PRODUCT_LEN + 1u];
    int group; /* index into net->groups, or -1 */
    NkLobbyEvent events[NK_NET_EVENT_QUEUE];
    unsigned ev_head, ev_count;
    uint32_t ev_dropped;
    Socket sockets[NK_NET_PDP_SOCKETS];
    uint32_t pdp_dropped;
} Console;

struct NkAdhocLoopNet {
    Console consoles[NK_LOOPNET_CONSOLES];
    Group groups[NK_LOOPNET_CONSOLES]; /* each console is in one group at most */
};

static Console *as_console(NkAdhocTransport *t) { return (Console *)t; }
static const Console *as_const_console(const NkAdhocTransport *t) { return (const Console *)t; }

static void push_event(Console *c, const NkLobbyEvent *ev) {
    if (c->ev_count == NK_NET_EVENT_QUEUE) {
        c->ev_dropped++;
        return;
    }
    c->events[(c->ev_head + c->ev_count) % NK_NET_EVENT_QUEUE] = *ev;
    c->ev_count++;
}

static void push_peer(Console *to, NkLobbyOpcode kind, const Console *peer) {
    NkLobbyEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    memcpy(ev.mac, peer->mac, NK_RELAY_MAC_LEN);
    memcpy(ev.nickname, peer->nickname, sizeof ev.nickname);
    ev.peer_id = peer->index + 1u;
    push_event(to, &ev);
}

/* Members of group g, in MAC order (the relay server keeps them in a map keyed by MAC). */
static unsigned members_in_mac_order(NkAdhocLoopNet *net, int g, Console *out[NK_LOOPNET_CONSOLES]) {
    unsigned n = 0;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++)
        if (net->consoles[i].logged_in && net->consoles[i].group == g) out[n++] = &net->consoles[i];
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && memcmp(out[j - 1]->mac, out[j]->mac, NK_RELAY_MAC_LEN) > 0; j--) {
            Console *tmp = out[j];
            out[j] = out[j - 1];
            out[j - 1] = tmp;
        }
    return n;
}

static void leave_group(Console *c) {
    if (c->group < 0) return;
    NkAdhocLoopNet *net = c->net;
    int g = c->group;
    c->group = -1;
    Console *members[NK_LOOPNET_CONSOLES];
    unsigned n = members_in_mac_order(net, g, members);
    for (unsigned i = 0; i < n; i++) {
        NkLobbyEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = NK_LOBBY_DISCONNECT;
        ev.peer_id = c->index + 1u;
        push_event(members[i], &ev);
    }
    if (n == 0) memset(&net->groups[g], 0, sizeof net->groups[g]);
}

static void free_queue(Socket *s) {
    for (unsigned i = 0; i < s->count; i++) free(s->queue[(s->head + i) % NK_NET_PDP_QUEUE].data);
    s->head = s->count = 0;
}

static int lo_login(NkAdhocTransport *t, const uint8_t mac[NK_RELAY_MAC_LEN], const char *nickname,
                    const char *product) {
    Console *c = as_console(t);
    if (c->logged_in || !mac || !nickname || !product) return NK_NET_BAD_ARG;
    if (!nk_relay_mac_ok(mac) || !nk_relay_product_ok(product)) return NK_NET_BAD_ARG;
    if (strlen(nickname) >= NK_RELAY_NICKNAME_LEN) return NK_NET_BAD_ARG;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        const Console *o = &c->net->consoles[i];
        if (o != c && o->logged_in && memcmp(o->mac, mac, NK_RELAY_MAC_LEN) == 0) return NK_NET_BAD_ARG;
    }
    memcpy(c->mac, mac, NK_RELAY_MAC_LEN);
    memset(c->nickname, 0, sizeof c->nickname);
    memcpy(c->nickname, nickname, strlen(nickname));
    memcpy(c->product, product, NK_RELAY_PRODUCT_LEN);
    c->product[NK_RELAY_PRODUCT_LEN] = '\0';
    c->group = -1;
    c->logged_in = 1;
    return NK_NET_OK;
}

static int lo_logout(NkAdhocTransport *t) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    leave_group(c);
    for (unsigned i = 0; i < NK_NET_PDP_SOCKETS; i++) {
        free_queue(&c->sockets[i]);
        c->sockets[i].used = 0;
    }
    c->ev_head = c->ev_count = 0;
    c->logged_in = 0;
    return NK_NET_OK;
}

static int lo_connect(NkAdhocTransport *t, const uint8_t group[NK_RELAY_GROUP_LEN]) {
    Console *c = as_console(t);
    if (!group) return NK_NET_BAD_ARG;
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    NkAdhocLoopNet *net = c->net;
    leave_group(c);
    int g = -1, free_slot = -1;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        Group *gr = &net->groups[i];
        if (!gr->used) {
            if (free_slot < 0) free_slot = (int)i;
        } else if (strcmp(gr->product, c->product) == 0 &&
                   memcmp(gr->name, group, NK_RELAY_GROUP_LEN) == 0) {
            g = (int)i;
        }
    }
    if (g < 0) {
        /* A free slot always exists: c has just left its group, so at most NK_LOOPNET_CONSOLES-1
         * groups are in use. */
        g = free_slot;
        Group *gr = &net->groups[g];
        gr->used = 1;
        memcpy(gr->product, c->product, sizeof gr->product);
        memcpy(gr->name, group, NK_RELAY_GROUP_LEN);
        memcpy(gr->leader, c->mac, NK_RELAY_MAC_LEN);
    }
    Console *members[NK_LOOPNET_CONSOLES];
    unsigned n = members_in_mac_order(net, g, members);
    for (unsigned i = 0; i < n; i++) {
        push_peer(members[i], NK_LOBBY_CONNECT, c);
        push_peer(c, NK_LOBBY_CONNECT, members[i]);
    }
    NkLobbyEvent ack;
    memset(&ack, 0, sizeof ack);
    ack.kind = NK_LOBBY_CONNECT_BSSID;
    memcpy(ack.mac, net->groups[g].leader, NK_RELAY_MAC_LEN);
    push_event(c, &ack);
    c->group = g;
    return NK_NET_OK;
}

static int lo_disconnect(NkAdhocTransport *t) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    leave_group(c);
    return NK_NET_OK;
}

static int lo_scan(NkAdhocTransport *t) {
    Console *c = as_console(t);
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    const Group *found[NK_LOOPNET_CONSOLES];
    unsigned n = 0;
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++) {
        const Group *gr = &c->net->groups[i];
        if (gr->used && strcmp(gr->product, c->product) == 0) found[n++] = gr;
    }
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && memcmp(found[j - 1]->name, found[j]->name, NK_RELAY_GROUP_LEN) > 0; j--) {
            const Group *tmp = found[j];
            found[j] = found[j - 1];
            found[j - 1] = tmp;
        }
    for (unsigned i = 0; i < n; i++) {
        NkLobbyEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = NK_LOBBY_SCAN;
        memcpy(ev.group, found[i]->name, NK_RELAY_GROUP_LEN);
        memcpy(ev.mac, found[i]->leader, NK_RELAY_MAC_LEN);
        push_event(c, &ev);
    }
    NkLobbyEvent done;
    memset(&done, 0, sizeof done);
    done.kind = NK_LOBBY_SCAN_COMPLETE;
    push_event(c, &done);
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

static const NkAdhocTransportOps s_loop_ops = {
    "loopback",
    lo_login, lo_logout, lo_connect, lo_disconnect, lo_scan, lo_take_events, lo_events_dropped,
    lo_pdp_open, lo_pdp_send, lo_pdp_recv, lo_pdp_close, lo_pdp_dropped,
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
    for (unsigned i = 0; i < NK_LOOPNET_CONSOLES; i++)
        for (unsigned k = 0; k < NK_NET_PDP_SOCKETS; k++) free_queue(&net->consoles[i].sockets[k]);
    free(net);
}

NkAdhocTransport *nk_loopnet_console(NkAdhocLoopNet *net, unsigned index) {
    if (!net || index >= NK_LOOPNET_CONSOLES) return NULL;
    return &net->consoles[index].base;
}
