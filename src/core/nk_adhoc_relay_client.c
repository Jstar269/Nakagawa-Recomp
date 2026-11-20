/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2012- PPSSPP Project */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * Opt-in plain-TCP ad hoc relay transport implementing NkAdhocTransportOps over
 * nk_adhoc_relay.h (#1139). Eager listener backlog up to max_backlog (RelayBacklog,
 * fillRelayBacklog, closeRelayBacklog) and three-way connect state polling
 * (RelayTransport::PtpConnect and RelayTransport::PtpConnectPoll, with the refused result they
 * report, SCE_NET_ADHOC_ERROR_CONNECTION_REFUSED; PTP_STATE_REFUSED is this file's name for it) are
 * adapted (derived-translated, docs/provenance/INDEPENDENCE_MODEL.md) from PPSSPP
 * Core/HLE/AdhocTransportRelay.cpp at revision 3bc60270eae3069f13dfbfdb7292a7e1375de6f6
 * (GPL-2.0-or-later); conveyed as GPL-3.0-or-later. Wire formats and limits arrive via
 * nk_adhoc_relay.h; no aemu_postoffice source code is imported or copied (aemu_postoffice
 * client/headers are GPL-3.0-only).
 *
 * Evidence label: PPSSPP-derived, not PSP-measured. No PSP was measured for what this file does;
 * it runs only when the user turns Online play on (SR_ADHOC_ENABLED=1), and src/rt/hle_network.c
 * logs the tier's first use once per run.
 *
 * Modified for Nakagawa Recomp (2026-11-16; see src/core/PROVENANCE.md):
 *   - C99 non-blocking Winsock2 / poll(2) TCP state machine with 512 KiB bounded TX/RX
 *     channel buffers directly encoding relay frames rather than wrapping blocking
 *     postoffice client calls.
 *   - Dead-lobby detection, single-lookup async DNS worker thread with AI_NUMERICHOST IP
 *     literal fast path, and PTP slot reservation before consuming connect frames.
 *   - 2026-11-17: the lobby tries each resolved server address in turn; the DNS worker holds
 *     its own Winsock reference and a destroyed client's pending lookup runs on detached.
 *   - 2026-11-20: a PDP socket whose queue is full leaves the rest of its stream unread instead
 *     of dropping datagrams; nk_relay_client_is_shut_down.
 */

#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "nk_adhoc_relay_client.h"

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
typedef SOCKET NkSockFd;
#  define NK_INVALID_SOCK INVALID_SOCKET
#  define NK_SEND_FLAGS 0
typedef CRITICAL_SECTION RelayLock;
#  define relay_lock_init(m) InitializeCriticalSection(m)
#  define relay_lock_destroy(m) DeleteCriticalSection(m)
#  define relay_lock_enter(m) EnterCriticalSection(m)
#  define relay_lock_leave(m) LeaveCriticalSection(m)
typedef HANDLE RelayThread;
#  define RELAY_THREAD_FN(name, arg) static DWORD WINAPI name(LPVOID arg)
#  define RELAY_THREAD_RETURN return 0u
static bool relay_thread_spawn(RelayThread *out, LPTHREAD_START_ROUTINE fn, void *arg) {
    HANDLE h = CreateThread(NULL, 0u, fn, arg, 0u, NULL);
    if (!h) return false;
    *out = h;
    return true;
}
static void relay_thread_finish(RelayThread t, bool wait) {
    if (wait) (void)WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
/* A reference on the host socket library (Winsock); each true return is paired with one
 * relay_net_unref. */
static bool relay_net_ref(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
static void relay_net_unref(void) { (void)WSACleanup(); }
/* A process-wide counter that any thread may change. */
typedef volatile LONG RelayCounter;
static void relay_counter_add(RelayCounter *c, int delta) {
    (void)InterlockedExchangeAdd(c, (LONG)delta);
}
static unsigned relay_counter_read(RelayCounter *c) {
    return (unsigned)InterlockedCompareExchange(c, 0, 0);
}
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <pthread.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <time.h>
#  include <unistd.h>
typedef int NkSockFd;
#  define NK_INVALID_SOCK (-1)
#  ifdef MSG_NOSIGNAL
#    define NK_SEND_FLAGS MSG_NOSIGNAL
#  else
#    define NK_SEND_FLAGS 0
#  endif
typedef pthread_mutex_t RelayLock;
#  define relay_lock_init(m) (void)pthread_mutex_init((m), NULL)
#  define relay_lock_destroy(m) (void)pthread_mutex_destroy(m)
#  define relay_lock_enter(m) (void)pthread_mutex_lock(m)
#  define relay_lock_leave(m) (void)pthread_mutex_unlock(m)
typedef pthread_t RelayThread;
#  define RELAY_THREAD_FN(name, arg) static void *name(void *arg)
#  define RELAY_THREAD_RETURN return NULL
static bool relay_thread_spawn(RelayThread *out, void *(*fn)(void *), void *arg) {
    return pthread_create(out, NULL, fn, arg) == 0;
}
static void relay_thread_finish(RelayThread t, bool wait) {
    if (wait) (void)pthread_join(t, NULL);
    else (void)pthread_detach(t);
}
/* POSIX sockets need no library reference. */
static bool relay_net_ref(void) { return true; }
static void relay_net_unref(void) {}
typedef unsigned RelayCounter;
static pthread_mutex_t s_relay_counter_lock = PTHREAD_MUTEX_INITIALIZER;
static void relay_counter_add(RelayCounter *c, int delta) {
    (void)pthread_mutex_lock(&s_relay_counter_lock);
    if (delta > 0) *c += (unsigned)delta;
    else *c -= (unsigned)-delta;
    (void)pthread_mutex_unlock(&s_relay_counter_lock);
}
static unsigned relay_counter_read(RelayCounter *c) {
    (void)pthread_mutex_lock(&s_relay_counter_lock);
    const unsigned n = *c;
    (void)pthread_mutex_unlock(&s_relay_counter_lock);
    return n;
}
#endif

#define NK_RELAY_CHAN_MAX_BUF (512u * 1024u)
#define NK_RELAY_PTP_SOCKETS 16u
#define NK_RELAY_PTP_BACKLOG 8u
#define NK_RELAY_PING_INTERVAL_MS UINT64_C(5000)
#define NK_RELAY_MAX_ADDRS 8u

/* The addresses a server name resolved to, in resolver order, each with the lobby and data
 * ports. The lobby tries them in turn until one connects (a name with AAAA and A records on a
 * host with no IPv6 route still reaches the server over IPv4). */
typedef struct RelayAddrList {
    unsigned count;
    struct sockaddr_storage lobby[NK_RELAY_MAX_ADDRS];
    socklen_t lobby_len[NK_RELAY_MAX_ADDRS];
    struct sockaddr_storage data[NK_RELAY_MAX_ADDRS];
    socklen_t data_len[NK_RELAY_MAX_ADDRS];
} RelayAddrList;

enum {
    PTP_STATE_UNUSED = 0,
    PTP_STATE_LISTEN = 1,
    PTP_STATE_CONNECT_WAIT_ACK = 2,
    PTP_STATE_ACCEPT_WAIT_ACK = 3,
    PTP_STATE_ESTABLISHED = 4,
    PTP_STATE_REFUSED = 5
};

typedef struct TcpChan {
    NkSockFd fd;
    int connecting;
    int connected;
    int remote_eof;
    int dead;
    uint8_t *tx;
    size_t tx_len;
    size_t tx_cap;
    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
} TcpChan;

typedef struct PdpDatagram {
    uint8_t mac[NK_RELAY_MAC_LEN];
    uint16_t port;
    uint32_t len;
    uint8_t *data;
} PdpDatagram;

typedef struct RelayPdpSocket {
    int used;
    uint16_t lport;
    TcpChan chan;
    PdpDatagram queue[NK_NET_PDP_QUEUE];
    unsigned head;
    unsigned count;
} RelayPdpSocket;

typedef struct RelayPtpSocket {
    int used;
    int state;
    uint16_t lport;
    uint16_t pport;
    uint8_t peer_mac[NK_RELAY_MAC_LEN];
    int max_backlog;
    int parent_listen;
    int pending_handles[NK_RELAY_PTP_BACKLOG];
    unsigned pending_count;
    TcpChan chan;
    uint8_t *app_rx;
    size_t app_rx_len;
    size_t app_rx_cap;
} RelayPtpSocket;

struct NkAdhocRelayClient {
    NkAdhocTransport base; /* first so base casts cleanly */
    char host[NK_ADHOC_SERVER_MAX_LEN + 1u];
    uint16_t lobby_port;
    uint16_t data_port;
    uint16_t port_offset;
    int wsa_started;

    struct sockaddr_storage lobby_addr;
    socklen_t lobby_addr_len;
    struct sockaddr_storage data_addr;
    socklen_t data_addr_len;
    RelayAddrList addrs;
    unsigned addr_next;
    int resolved;
    struct RelayResolveJob *resolve_job;

    int logged_in;
    int in_group;
    int scanning;
    uint8_t mac[NK_RELAY_MAC_LEN];
    char nickname[NK_RELAY_NICKNAME_LEN + 1u];
    char product[NK_RELAY_PRODUCT_LEN + 1u];

    TcpChan lobby;
    uint64_t last_lobby_tx_ms;
    NkLobbyEvent events[NK_NET_EVENT_QUEUE];
    unsigned ev_head;
    unsigned ev_count;
    uint32_t ev_dropped;

    RelayPdpSocket pdp[NK_NET_PDP_SOCKETS];
    RelayPtpSocket ptp[NK_RELAY_PTP_SOCKETS];
    uint16_t next_ptp_port;
    int shut_down; /* nk_relay_client_shutdown: every socket closed, none opened again */
    char last_error[160];
};

static NkAdhocRelayClient *as_client(NkAdhocTransport *t) { return (NkAdhocRelayClient *)t; }
static const NkAdhocRelayClient *as_const_client(const NkAdhocTransport *t) {
    return (const NkAdhocRelayClient *)t;
}

static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

static int sock_last_err(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool err_would_block(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS || err == EALREADY;
#endif
}

static void sock_close_fd(NkSockFd fd) {
    if (fd == NK_INVALID_SOCK) return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static bool sock_set_nonblock_nodelay(NkSockFd fd) {
#ifdef _WIN32
    u_long on = 1u;
    if (ioctlsocket(fd, FIONBIO, &on) != 0) return false;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
#endif
    int nodelay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    return true;
}

static void set_error(NkAdhocRelayClient *c, const char *msg) {
    if (!c) return;
    snprintf(c->last_error, sizeof(c->last_error), "%s", msg ? msg : "");
}

static void chan_init(TcpChan *ch) {
    memset(ch, 0, sizeof *ch);
    ch->fd = NK_INVALID_SOCK;
}

static void chan_close(TcpChan *ch) {
    if (!ch) return;
    sock_close_fd(ch->fd);
    ch->fd = NK_INVALID_SOCK;
    ch->connecting = 0;
    ch->connected = 0;
    ch->remote_eof = 0;
    ch->dead = 0;
    free(ch->tx);
    ch->tx = NULL;
    ch->tx_len = 0u;
    ch->tx_cap = 0u;
    free(ch->rx);
    ch->rx = NULL;
    ch->rx_len = 0u;
    ch->rx_cap = 0u;
}

static bool chan_connect_existing_tx(TcpChan *ch, const struct sockaddr *addr, socklen_t len) {
    NkSockFd fd = socket(addr->sa_family, SOCK_STREAM, 0);
    if (fd == NK_INVALID_SOCK) {
        ch->dead = 1;
        return false;
    }
    if (!sock_set_nonblock_nodelay(fd)) {
        sock_close_fd(fd);
        ch->dead = 1;
        return false;
    }
    int rc = connect(fd, addr, len);
    if (rc == 0) {
        ch->fd = fd;
        ch->connected = 1;
        return true;
    }
    int err = sock_last_err();
    if (err_would_block(err)) {
        ch->fd = fd;
        ch->connecting = 1;
        return true;
    }
    sock_close_fd(fd);
    ch->dead = 1;
    return false;
}

static bool buf_append(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *data, size_t n,
                       size_t limit) {
    if (n == 0u) return true;
    if (!data || *len > limit || n > limit - *len) return false;
    const size_t need = *len + n;
    if (need > *cap) {
        size_t next = *cap ? *cap * 2u : 4096u;
        while (next < need) next *= 2u;
        if (next > limit) next = limit;
        uint8_t *grown = realloc(*buf, next);
        if (!grown) return false;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, data, n);
    *len = need;
    return true;
}

static void chan_poll_connect(TcpChan *ch) {
    if (!ch->connecting || ch->fd == NK_INVALID_SOCK || ch->dead) return;
    int ready = 0;
#ifdef _WIN32
    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(ch->fd, &wfds);
    FD_SET(ch->fd, &efds);
    struct timeval tv = {0, 0};
    int rc = select(0, NULL, &wfds, &efds, &tv);
    if (rc <= 0) return;
    ready = (FD_ISSET(ch->fd, &wfds) || FD_ISSET(ch->fd, &efds)) ? 1 : 0;
#else
    struct pollfd pfd;
    pfd.fd = ch->fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int rc = poll(&pfd, 1, 0);
    if (rc <= 0) return;
    ready = (pfd.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) != 0 ? 1 : 0;
#endif
    int err = 0;
    socklen_t elen = (socklen_t)sizeof(err);
    if (getsockopt(ch->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 || err != 0) {
        ch->connecting = 0;
        ch->dead = 1;
        return;
    }
    if (ready) {
        ch->connecting = 0;
        ch->connected = 1;
    }
}

static void chan_flush_tx(TcpChan *ch) {
    chan_poll_connect(ch);
    if (!ch->connected || ch->dead || ch->fd == NK_INVALID_SOCK) return;
    while (ch->tx_len > 0u) {
#ifdef _WIN32
        const int sent = send(ch->fd, (const char *)ch->tx, (int)ch->tx_len, NK_SEND_FLAGS);
#else
        const ssize_t sent = send(ch->fd, ch->tx, ch->tx_len, NK_SEND_FLAGS);
#endif
        if (sent > 0) {
            const size_t n = (size_t)sent;
            if (n < ch->tx_len) memmove(ch->tx, ch->tx + n, ch->tx_len - n);
            ch->tx_len -= n;
            continue;
        }
        if (sent < 0 && err_would_block(sock_last_err())) break;
        ch->dead = 1;
        break;
    }
}

static void chan_pump_rx(TcpChan *ch) {
    chan_poll_connect(ch);
    if (!ch->connected || ch->dead || ch->remote_eof || ch->fd == NK_INVALID_SOCK) return;
    uint8_t tmp[4096];
    for (;;) {
        if (ch->rx_len >= NK_RELAY_CHAN_MAX_BUF) break;
        const size_t room = NK_RELAY_CHAN_MAX_BUF - ch->rx_len;
        const size_t want = room < sizeof(tmp) ? room : sizeof(tmp);
#ifdef _WIN32
        const int got = recv(ch->fd, (char *)tmp, (int)want, 0);
#else
        const ssize_t got = recv(ch->fd, tmp, want, 0);
#endif
        if (got > 0) {
            if (!buf_append(&ch->rx, &ch->rx_len, &ch->rx_cap, tmp, (size_t)got,
                            NK_RELAY_CHAN_MAX_BUF)) {
                ch->dead = 1;
                break;
            }
            if ((size_t)got < want) break;
            continue;
        }
        if (got == 0) {
            ch->remote_eof = 1;
            break;
        }
        if (err_would_block(sock_last_err())) break;
        ch->dead = 1;
        break;
    }
}

static void chan_consume_rx(TcpChan *ch, size_t n) {
    if (n == 0u) return;
    if (n >= ch->rx_len) {
        ch->rx_len = 0u;
        return;
    }
    memmove(ch->rx, ch->rx + n, ch->rx_len - n);
    ch->rx_len -= n;
}

static NkRelayTestResolveFn s_test_resolve_fn;
static void *s_test_resolve_ctx;

void nk_relay_client_set_resolve_hook_for_test(NkRelayTestResolveFn fn, void *ctx) {
    s_test_resolve_fn = fn;
    s_test_resolve_ctx = ctx;
}

static void assign_sockaddr_ports(const struct sockaddr *src, socklen_t src_len,
                                  uint16_t lobby_port, uint16_t data_port,
                                  struct sockaddr_storage *lobby_out, socklen_t *lobby_len,
                                  struct sockaddr_storage *data_out, socklen_t *data_len) {
    if (src->sa_family == AF_INET) {
        struct sockaddr_in sin;
        memcpy(&sin, src, sizeof(sin));
        sin.sin_port = htons(lobby_port);
        memcpy(lobby_out, &sin, sizeof(sin));
        *lobby_len = (socklen_t)sizeof(sin);
        sin.sin_port = htons(data_port);
        memcpy(data_out, &sin, sizeof(sin));
        *data_len = (socklen_t)sizeof(sin);
    } else {
        struct sockaddr_in6 sin6;
        memcpy(&sin6, src, sizeof(sin6));
        sin6.sin6_port = htons(lobby_port);
        memcpy(lobby_out, &sin6, sizeof(sin6));
        *lobby_len = (socklen_t)sizeof(sin6);
        sin6.sin6_port = htons(data_port);
        memcpy(data_out, &sin6, sizeof(sin6));
        *data_len = (socklen_t)sizeof(sin6);
    }
    (void)src_len;
}

static bool resolve_host_addrs(const char *host, uint16_t lobby_port, uint16_t data_port,
                               int extra_flags, NkRelayTestResolveFn hook_fn, void *hook_ctx,
                               RelayAddrList *out) {
    out->count = 0u;
    if (hook_fn) {
        uint8_t ip4[4] = {0};
        if (!hook_fn(host, ip4, hook_ctx)) return false;
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        memcpy(&sin.sin_addr, ip4, 4u);
        assign_sockaddr_ports((const struct sockaddr *)&sin, (socklen_t)sizeof(sin),
                              lobby_port, data_port, &out->lobby[0], &out->lobby_len[0],
                              &out->data[0], &out->data_len[0]);
        out->count = 1u;
        return true;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = extra_flags;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)lobby_port);
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return false;
    for (struct addrinfo *it = res; it && out->count < NK_RELAY_MAX_ADDRS; it = it->ai_next) {
        if ((it->ai_family == AF_INET && it->ai_addrlen >= sizeof(struct sockaddr_in)) ||
            (it->ai_family == AF_INET6 && it->ai_addrlen >= sizeof(struct sockaddr_in6))) {
            if (it->ai_addrlen <= sizeof(struct sockaddr_storage)) {
                const unsigned i = out->count++;
                assign_sockaddr_ports(it->ai_addr, (socklen_t)it->ai_addrlen, lobby_port, data_port,
                                      &out->lobby[i], &out->lobby_len[i], &out->data[i],
                                      &out->data_len[i]);
            }
        }
    }
    freeaddrinfo(res);
    return out->count > 0u;
}

typedef struct RelayResolveJob {
    RelayLock lock;
    RelayThread thread;
    int thread_valid;
    int refs;
    int done;
    int ok;
    char host[NK_ADHOC_SERVER_MAX_LEN + 1u];
    uint16_t lobby_port;
    uint16_t data_port;
    NkRelayTestResolveFn hook_fn;
    void *hook_ctx;
    RelayAddrList addrs;
    int net_ref; /* the job's own socket-library reference, dropped by the worker as it ends */
} RelayResolveJob;

static RelayCounter s_resolve_jobs_live;

unsigned nk_relay_client_resolve_jobs_live_for_test(void) {
    return relay_counter_read(&s_resolve_jobs_live);
}

static void free_resolve_job(RelayResolveJob *job) {
    relay_lock_destroy(&job->lock);
    free(job);
    relay_counter_add(&s_resolve_jobs_live, -1);
}

RELAY_THREAD_FN(resolve_worker_fn, raw) {
    RelayResolveJob *job = (RelayResolveJob *)raw;
    const int net_ref = job->net_ref;
    RelayAddrList found;
    memset(&found, 0, sizeof(found));
    const bool ok = resolve_host_addrs(job->host, job->lobby_port, job->data_port, 0, job->hook_fn,
                                       job->hook_ctx, &found);
    relay_lock_enter(&job->lock);
    if (ok) {
        job->addrs = found;
        job->ok = 1;
    }
    job->done = 1;
    job->refs--;
    const int drop = (job->refs == 0);
    relay_lock_leave(&job->lock);
    if (drop) free_resolve_job(job);
    /* A destroyed client has already dropped its reference; this one kept the socket library
     * (Winsock) up for the lookup until here. */
    if (net_ref) relay_net_unref();
    RELAY_THREAD_RETURN;
}

static RelayResolveJob *start_resolve_job(const char *host, uint16_t lobby_port, uint16_t data_port,
                                          NkRelayTestResolveFn hook_fn, void *hook_ctx) {
    RelayResolveJob *job = calloc(1, sizeof *job);
    if (!job) return NULL;
    relay_counter_add(&s_resolve_jobs_live, 1);
    relay_lock_init(&job->lock);
    job->refs = 2;
    snprintf(job->host, sizeof(job->host), "%s", host);
    job->lobby_port = lobby_port;
    job->data_port = data_port;
    job->hook_fn = hook_fn;
    job->hook_ctx = hook_ctx;
    /* The lookup may outlive the client (release_resolve_job detaches it), so it holds its own
     * socket-library (Winsock) reference rather than leaning on the client's. */
    if (!relay_net_ref()) {
        free_resolve_job(job);
        return NULL;
    }
    job->net_ref = 1;
    if (!relay_thread_spawn(&job->thread, resolve_worker_fn, job)) {
        relay_net_unref();
        free_resolve_job(job);
        return NULL;
    }
    job->thread_valid = 1;
    return job;
}

static void release_resolve_job(RelayResolveJob **slot) {
    RelayResolveJob *job = *slot;
    if (!job) return;
    *slot = NULL;
    relay_lock_enter(&job->lock);
    const int done = job->done;
    const int tv = job->thread_valid;
    const RelayThread th = job->thread;
    job->thread_valid = 0;
    if (done) {
        relay_lock_leave(&job->lock);
        if (tv) relay_thread_finish(th, true);
        free_resolve_job(job);
        return;
    }
    job->refs--;
    const int drop = (job->refs == 0);
    relay_lock_leave(&job->lock);
    if (tv) relay_thread_finish(th, false);
    if (drop) free_resolve_job(job);
}

static void queue_lobby_event(NkAdhocRelayClient *c, const NkLobbyEvent *ev) {
    if (c->ev_count == NK_NET_EVENT_QUEUE) {
        c->ev_dropped++;
        return;
    }
    c->events[(c->ev_head + c->ev_count) % NK_NET_EVENT_QUEUE] = *ev;
    c->ev_count++;
}

static void free_pdp_queue(RelayPdpSocket *s) {
    for (unsigned i = 0u; i < s->count; i++) {
        PdpDatagram *d = &s->queue[(s->head + i) % NK_NET_PDP_QUEUE];
        free(d->data);
        d->data = NULL;
    }
    s->head = 0u;
    s->count = 0u;
}

static void close_pdp_slot(RelayPdpSocket *s) {
    free_pdp_queue(s);
    chan_close(&s->chan);
    s->used = 0;
    s->lport = 0u;
}

static void close_ptp_slot(NkAdhocRelayClient *c, int handle) {
    if (handle < 0 || (unsigned)handle >= NK_RELAY_PTP_SOCKETS) return;
    RelayPtpSocket *ps = &c->ptp[handle];
    if (!ps->used) return;
    if (ps->state == PTP_STATE_LISTEN) {
        for (unsigned i = 0u; i < ps->pending_count; i++) {
            close_ptp_slot(c, ps->pending_handles[i]);
        }
        for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) {
            if (c->ptp[i].used && c->ptp[i].state == PTP_STATE_ACCEPT_WAIT_ACK &&
                c->ptp[i].parent_listen == handle) {
                close_ptp_slot(c, (int)i);
            }
        }
    }
    chan_close(&ps->chan);
    free(ps->app_rx);
    memset(ps, 0, sizeof *ps);
    ps->parent_listen = -1;
}

static void close_all_sockets(NkAdhocRelayClient *c) {
    for (unsigned i = 0u; i < NK_NET_PDP_SOCKETS; i++) close_pdp_slot(&c->pdp[i]);
    for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) close_ptp_slot(c, (int)i);
}

static int alloc_ptp_slot(NkAdhocRelayClient *c) {
    for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) {
        if (!c->ptp[i].used) {
            RelayPtpSocket *ps = &c->ptp[i];
            memset(ps, 0, sizeof *ps);
            chan_init(&ps->chan);
            ps->used = 1;
            ps->parent_listen = -1;
            return (int)i;
        }
    }
    return NK_NET_NO_SLOT;
}

/* Any socket on the port: an automatic port skips it. */
static bool ptp_port_in_use(const NkAdhocRelayClient *c, uint16_t port) {
    for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) {
        if (c->ptp[i].used && c->ptp[i].state != PTP_STATE_UNUSED && c->ptp[i].lport == port)
            return true;
    }
    return false;
}

/* The ports a listen or an open the caller named is refused on, by the HLE layer's rule
 * (src/rt/hle_network.c adhoc_ptp_port_in_use, after PPSSPP's isPTPPortInUse): a listen when
 * another listener holds the port, an open when a stream already joins the same local port, peer
 * MAC and peer port. A listener, its accepted children and streams to other peers share a port. */
static bool ptp_listen_port_taken(const NkAdhocRelayClient *c, uint16_t port) {
    for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) {
        const RelayPtpSocket *ps = &c->ptp[i];
        if (ps->used && ps->state == PTP_STATE_LISTEN && ps->lport == port) return true;
    }
    return false;
}

static bool ptp_stream_taken(const NkAdhocRelayClient *c, uint16_t port,
                             const uint8_t dst[NK_RELAY_MAC_LEN], uint16_t dport) {
    for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) {
        const RelayPtpSocket *ps = &c->ptp[i];
        if (ps->used && ps->state != PTP_STATE_UNUSED && ps->state != PTP_STATE_LISTEN &&
            ps->lport == port && ps->pport == dport &&
            memcmp(ps->peer_mac, dst, NK_RELAY_MAC_LEN) == 0)
            return true;
    }
    return false;
}

static uint16_t pick_ptp_port(NkAdhocRelayClient *c) {
    if (c->next_ptp_port < 0x4000u) c->next_ptp_port = 0x4000u;
    for (unsigned n = 0u; n < 16384u; n++) {
        const uint16_t candidate = c->next_ptp_port++;
        if (c->next_ptp_port < 0x4000u) c->next_ptp_port = 0x4000u;
        if (candidate != 0u && candidate != 0xffffu && !ptp_port_in_use(c, candidate))
            return candidate;
    }
    return 0u;
}

/* True while the lobby has not connected and a later resolved address is left to try. */
static bool lobby_has_fallback(const NkAdhocRelayClient *c) {
    return c->resolved && !c->lobby.connected && c->addr_next < c->addrs.count;
}

/* The server address is settled once the lobby connected or no other address is left; data
 * channels connect to it only then, so they follow the lobby to the address that answers. */
static bool addr_committed(const NkAdhocRelayClient *c) {
    return c->resolved && !lobby_has_fallback(c);
}

/* Points the lobby at the next resolved address and starts its connect, keeping the frames
 * already queued (the login). Returns false, with the lobby dead, when no address is left. */
static bool lobby_connect_next(NkAdhocRelayClient *c) {
    while (c->addr_next < c->addrs.count) {
        const unsigned i = c->addr_next++;
        c->lobby_addr = c->addrs.lobby[i];
        c->lobby_addr_len = c->addrs.lobby_len[i];
        c->data_addr = c->addrs.data[i];
        c->data_addr_len = c->addrs.data_len[i];
        sock_close_fd(c->lobby.fd);
        c->lobby.fd = NK_INVALID_SOCK;
        c->lobby.connecting = 0;
        c->lobby.connected = 0;
        c->lobby.remote_eof = 0;
        c->lobby.dead = 0;
        c->lobby.rx_len = 0u;
        if (chan_connect_existing_tx(&c->lobby, (const struct sockaddr *)&c->lobby_addr,
                                     c->lobby_addr_len))
            return true;
    }
    c->lobby.dead = 1;
    return false;
}

/* A lobby connect that failed before it ever connected moves on to the next address. */
static void lobby_fall_back(NkAdhocRelayClient *c) {
    if (c->lobby.dead && lobby_has_fallback(c)) (void)lobby_connect_next(c);
}

static bool ensure_data_chan_connected(NkAdhocRelayClient *c, TcpChan *ch) {
    if (!addr_committed(c)) {
        if (c->lobby.dead && !lobby_has_fallback(c)) ch->dead = 1;
        return false;
    }
    if (ch->fd == NK_INVALID_SOCK && !ch->dead && !ch->remote_eof) {
        if (!chan_connect_existing_tx(ch, (const struct sockaddr *)&c->data_addr,
                                      c->data_addr_len)) {
            return false;
        }
    }
    return true;
}

/* Decodes the lobby frames received so far while the event queue has room. A frame the queue
 * cannot hold stays in lobby.rx: the receive buffer then fills, chan_pump_rx stops reading at
 * NK_RELAY_CHAN_MAX_BUF and TCP holds the rest at the server, so a burst (a scan answer of more
 * groups than the queue holds) waits instead of losing its last frames. rc_take_events calls this
 * again as it drains the queue. A frame that does not parse ends the session and the bytes after
 * it are discarded, since nothing after it can be framed. */
static void parse_lobby_rx(NkAdhocRelayClient *c) {
    while (c->lobby.rx_len > 0u && c->ev_count < NK_NET_EVENT_QUEUE) {
        NkLobbyEvent ev;
        const int used = nk_lobby_parse(c->lobby.rx, c->lobby.rx_len, &ev);
        if (used == NK_LOBBY_NEED_MORE) break;
        if (used == NK_LOBBY_BAD) {
            c->lobby.dead = 1;
            c->lobby.rx_len = 0u;
            set_error(c, "lobby rejected invalid server frame");
            break;
        }
        chan_consume_rx(&c->lobby, (size_t)used);
        if (ev.kind == NK_LOBBY_CONNECT_BSSID) c->in_group = 1;
        if (ev.kind == NK_LOBBY_SCAN_COMPLETE) c->scanning = 0;
        if (ev.kind != NK_LOBBY_CHAT) queue_lobby_event(c, &ev);
    }
}

static void pump_lobby(NkAdhocRelayClient *c) {
    if (!c->logged_in || c->lobby.dead) return;
    if (!c->resolved) {
        RelayResolveJob *job = c->resolve_job;
        if (!job) {
            c->lobby.dead = 1;
            set_error(c, "could not resolve ad hoc server");
            return;
        }
        relay_lock_enter(&job->lock);
        const int done = job->done;
        const int ok = job->ok;
        if (done && ok) c->addrs = job->addrs;
        relay_lock_leave(&job->lock);
        if (!done) return;
        release_resolve_job(&c->resolve_job);
        if (!ok) {
            c->lobby.dead = 1;
            set_error(c, "could not resolve ad hoc server");
            return;
        }
        c->resolved = 1;
        c->addr_next = 0u;
        if (!lobby_connect_next(c)) {
            set_error(c, "could not connect to lobby server");
            return;
        }
        c->last_lobby_tx_ms = monotonic_ms();
    }
    const uint64_t now = monotonic_ms();
    if (c->lobby.connected && now - c->last_lobby_tx_ms >= NK_RELAY_PING_INTERVAL_MS &&
        c->lobby.tx_len == 0u) {
        uint8_t ping[NK_LOBBY_BARE_SIZE];
        const size_t n = nk_lobby_bare(ping, NK_LOBBY_PING);
        if (n > 0u && buf_append(&c->lobby.tx, &c->lobby.tx_len, &c->lobby.tx_cap, ping, n,
                                 NK_RELAY_CHAN_MAX_BUF)) {
            c->last_lobby_tx_ms = now;
        }
    }
    chan_flush_tx(&c->lobby);
    lobby_fall_back(c);
    chan_pump_rx(&c->lobby);
    lobby_fall_back(c);
    parse_lobby_rx(c);
    if (c->lobby.remote_eof || c->lobby.dead) {
        if (!c->last_error[0])
            set_error(c, c->lobby.connected ? "lobby connection closed" : "lobby connection failed");
        c->lobby.dead = 1;
    }
}

static bool open_data_chan(NkAdhocRelayClient *c, TcpChan *ch, NkRelayInit type, uint16_t sport,
                           const uint8_t *dst_mac, uint16_t dport) {
    uint8_t init[NK_RELAY_INIT_SIZE];
    const uint16_t wire_sport = nk_relay_port_out(sport, c->port_offset);
    const uint16_t wire_dport = nk_relay_port_out(dport, c->port_offset);
    if (c->shut_down) return false;
    if (nk_relay_init(init, type, c->mac, wire_sport, dst_mac, wire_dport) != NK_RELAY_INIT_SIZE)
        return false;
    chan_close(ch);
    if (!buf_append(&ch->tx, &ch->tx_len, &ch->tx_cap, init, sizeof(init), NK_RELAY_CHAN_MAX_BUF)) {
        chan_close(ch);
        ch->dead = 1;
        return false;
    }
    if (addr_committed(c)) {
        if (!chan_connect_existing_tx(ch, (const struct sockaddr *)&c->data_addr,
                                      c->data_addr_len)) {
            chan_close(ch);
            ch->dead = 1;
            return false;
        }
        chan_flush_tx(ch);
        return !ch->dead;
    }
    if ((c->resolve_job || lobby_has_fallback(c)) && !c->lobby.dead) return true;
    chan_close(ch);
    ch->dead = 1;
    return false;
}

/* Decodes the PDP frames received so far while the socket's queue has room, as parse_lobby_rx does
 * for the lobby: a frame the queue cannot hold stays in the receive buffer, chan_pump_rx stops
 * reading at NK_RELAY_CHAN_MAX_BUF, and TCP holds the rest at the relay until the game reads.
 * No datagram is dropped here. Frames received before the relay closed the stream are still
 * delivered. A frame that does not parse, or one there is no memory to copy, ends the stream by
 * name (last error), and pdp_recv then answers NK_NET_DEAD. */
static void parse_pdp_rx(NkAdhocRelayClient *c, RelayPdpSocket *s) {
    while (!s->chan.dead && s->chan.rx_len >= NK_RELAY_PDP_HEADER_SIZE &&
           s->count < NK_NET_PDP_QUEUE) {
        uint8_t src_mac[NK_RELAY_MAC_LEN];
        uint16_t wire_port = 0u;
        uint32_t plen = 0u;
        if (!nk_relay_parse_pdp_header(s->chan.rx, src_mac, &wire_port, &plen)) {
            s->chan.dead = 1;
            set_error(c, "relay rejected oversized PDP frame");
            break;
        }
        const size_t total = NK_RELAY_PDP_HEADER_SIZE + (size_t)plen;
        if (s->chan.rx_len < total) break;
        const uint16_t app_port = nk_relay_port_in(wire_port, c->port_offset);
        uint8_t *copy = plen ? malloc(plen) : NULL;
        if (plen && !copy) {
            s->chan.dead = 1;
            set_error(c, "out of memory for a PDP datagram");
            break;
        }
        if (plen) memcpy(copy, s->chan.rx + NK_RELAY_PDP_HEADER_SIZE, plen);
        PdpDatagram *d = &s->queue[(s->head + s->count) % NK_NET_PDP_QUEUE];
        memcpy(d->mac, src_mac, NK_RELAY_MAC_LEN);
        d->port = app_port;
        d->len = plen;
        d->data = copy;
        s->count++;
        chan_consume_rx(&s->chan, total);
    }
}

static void pump_pdp_socket(NkAdhocRelayClient *c, RelayPdpSocket *s) {
    if (!s->used) return;
    if (!s->chan.dead && !s->chan.remote_eof && ensure_data_chan_connected(c, &s->chan)) {
        chan_flush_tx(&s->chan);
        chan_pump_rx(&s->chan);
    }
    parse_pdp_rx(c, s);
}

static unsigned count_accepting_for(const NkAdhocRelayClient *c, int listen_handle) {
    unsigned n = 0u;
    for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) {
        if (c->ptp[i].used && c->ptp[i].state == PTP_STATE_ACCEPT_WAIT_ACK &&
            c->ptp[i].parent_listen == listen_handle)
            n++;
    }
    return n;
}

static void pump_ptp_socket(NkAdhocRelayClient *c, int idx) {
    RelayPtpSocket *ps = &c->ptp[idx];
    if (!ps->used || ps->state == PTP_STATE_UNUSED || ps->state == PTP_STATE_REFUSED) return;
    if (!ps->chan.dead && !ps->chan.remote_eof && !ensure_data_chan_connected(c, &ps->chan) &&
        !addr_committed(c))
        return;

    chan_flush_tx(&ps->chan);
    chan_pump_rx(&ps->chan);

    if (ps->state == PTP_STATE_LISTEN) {
        while (ps->chan.rx_len >= NK_RELAY_PTP_CONNECT_SIZE) {
            const unsigned inflight = ps->pending_count + count_accepting_for(c, idx);
            if ((int)inflight >= ps->max_backlog) break;
            uint8_t peer_mac[NK_RELAY_MAC_LEN];
            uint16_t wire_pport = 0u;
            if (!nk_relay_parse_ptp_connect(ps->chan.rx, peer_mac, &wire_pport)) break;
            const int child_idx = alloc_ptp_slot(c);
            if (child_idx < 0) break;
            chan_consume_rx(&ps->chan, NK_RELAY_PTP_CONNECT_SIZE);
            const uint16_t app_pport = nk_relay_port_in(wire_pport, c->port_offset);
            RelayPtpSocket *child = &c->ptp[child_idx];
            child->state = PTP_STATE_ACCEPT_WAIT_ACK;
            child->lport = ps->lport;
            child->pport = app_pport;
            memcpy(child->peer_mac, peer_mac, NK_RELAY_MAC_LEN);
            child->parent_listen = idx;
            if (!open_data_chan(c, &child->chan, NK_RELAY_INIT_PTP_ACCEPT, ps->lport, peer_mac,
                                app_pport)) {
                close_ptp_slot(c, child_idx);
            }
        }
        return;
    }

    if (ps->state == PTP_STATE_CONNECT_WAIT_ACK || ps->state == PTP_STATE_ACCEPT_WAIT_ACK) {
        if (ps->chan.rx_len >= NK_RELAY_PTP_CONNECT_SIZE) {
            uint8_t ack_mac[NK_RELAY_MAC_LEN];
            uint16_t ack_wire_port = 0u;
            (void)nk_relay_parse_ptp_connect(ps->chan.rx, ack_mac, &ack_wire_port);
            chan_consume_rx(&ps->chan, NK_RELAY_PTP_CONNECT_SIZE);
            const int was_accept = (ps->state == PTP_STATE_ACCEPT_WAIT_ACK);
            const int parent = ps->parent_listen;
            ps->state = PTP_STATE_ESTABLISHED;
            ps->parent_listen = -1;
            if (was_accept && parent >= 0 && (unsigned)parent < NK_RELAY_PTP_SOCKETS &&
                c->ptp[parent].used && c->ptp[parent].state == PTP_STATE_LISTEN &&
                c->ptp[parent].pending_count < NK_RELAY_PTP_BACKLOG) {
                c->ptp[parent].pending_handles[c->ptp[parent].pending_count++] = idx;
            }
        } else if (ps->chan.dead || ps->chan.remote_eof) {
            if (ps->state == PTP_STATE_ACCEPT_WAIT_ACK) {
                close_ptp_slot(c, idx);
            } else {
                ps->state = PTP_STATE_REFUSED;
            }
            return;
        }
    }

    if (ps->state == PTP_STATE_ESTABLISHED) {
        while (ps->chan.rx_len >= NK_RELAY_PTP_DATA_HEADER_SIZE) {
            uint32_t blen = 0u;
            if (!nk_relay_parse_ptp_data_header(ps->chan.rx, &blen)) {
                ps->chan.dead = 1;
                set_error(c, "relay rejected oversized PTP block");
                break;
            }
            const size_t total = NK_RELAY_PTP_DATA_HEADER_SIZE + (size_t)blen;
            if (ps->chan.rx_len < total) break;
            if (ps->app_rx_len + blen > NK_RELAY_CHAN_MAX_BUF) break;
            if (!buf_append(&ps->app_rx, &ps->app_rx_len, &ps->app_rx_cap,
                            ps->chan.rx + NK_RELAY_PTP_DATA_HEADER_SIZE, blen,
                            NK_RELAY_CHAN_MAX_BUF)) {
                ps->chan.dead = 1;
                break;
            }
            chan_consume_rx(&ps->chan, total);
        }
    }
}

static void rc_pump(NkAdhocTransport *t) {
    NkAdhocRelayClient *c = as_client(t);
    if (!c || !c->logged_in) return;
    pump_lobby(c);
    for (unsigned i = 0u; i < NK_NET_PDP_SOCKETS; i++) pump_pdp_socket(c, &c->pdp[i]);
    for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) pump_ptp_socket(c, (int)i);
}

static int rc_login(NkAdhocTransport *t, const uint8_t mac[NK_RELAY_MAC_LEN], const char *nickname,
                    const char *product) {
    NkAdhocRelayClient *c = as_client(t);
    if (!c || !mac || !nickname || !product) return NK_NET_BAD_ARG;
    uint8_t frame[NK_LOBBY_LOGIN_SIZE];
    if (nk_lobby_login(frame, mac, nickname, product) != NK_LOBBY_LOGIN_SIZE) return NK_NET_BAD_ARG;
    /* A client that was shut down stays down: its reason stays the last error. */
    if (c->shut_down) return NK_NET_DEAD;

    if (c->logged_in) {
        release_resolve_job(&c->resolve_job);
        close_all_sockets(c);
        chan_close(&c->lobby);
        c->logged_in = 0;
        c->in_group = 0;
        c->scanning = 0;
        c->resolved = 0;
    }

    c->last_error[0] = '\0';
    chan_close(&c->lobby);
    release_resolve_job(&c->resolve_job);
    c->resolved = 0;
    c->addr_next = 0u;
    if (!s_test_resolve_fn &&
        resolve_host_addrs(c->host, c->lobby_port, c->data_port, AI_NUMERICHOST, NULL, NULL,
                           &c->addrs)) {
        c->resolved = 1;
    } else {
        c->resolve_job = start_resolve_job(c->host, c->lobby_port, c->data_port,
                                           s_test_resolve_fn, s_test_resolve_ctx);
        if (!c->resolve_job) {
            set_error(c, "could not resolve ad hoc server");
            return NK_NET_DEAD;
        }
    }

    if (c->resolved && !lobby_connect_next(c)) {
        set_error(c, "could not connect to lobby server");
        return NK_NET_DEAD;
    }
    if (!buf_append(&c->lobby.tx, &c->lobby.tx_len, &c->lobby.tx_cap, frame, sizeof(frame),
                    NK_RELAY_CHAN_MAX_BUF)) {
        release_resolve_job(&c->resolve_job);
        chan_close(&c->lobby);
        set_error(c, "lobby send buffer full");
        return NK_NET_DEAD;
    }
    memcpy(c->mac, mac, NK_RELAY_MAC_LEN);
    snprintf(c->nickname, sizeof(c->nickname), "%s", nickname);
    snprintf(c->product, sizeof(c->product), "%s", product);
    c->logged_in = 1;
    c->in_group = 0;
    c->scanning = 0;
    c->ev_head = 0u;
    c->ev_count = 0u;
    c->ev_dropped = 0u;
    c->last_lobby_tx_ms = monotonic_ms();
    rc_pump(t);
    return NK_NET_OK;
}

static int queue_lobby_cmd(NkAdhocRelayClient *c, const uint8_t *bytes, size_t n) {
    if (c->shut_down) return NK_NET_DEAD;
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (c->lobby.dead || c->lobby.remote_eof) return NK_NET_DEAD;
    if (!buf_append(&c->lobby.tx, &c->lobby.tx_len, &c->lobby.tx_cap, bytes, n,
                    NK_RELAY_CHAN_MAX_BUF))
        return NK_NET_AGAIN;
    c->last_lobby_tx_ms = monotonic_ms();
    chan_flush_tx(&c->lobby);
    lobby_fall_back(c);
    return NK_NET_OK;
}

static int rc_logout(NkAdhocTransport *t) {
    NkAdhocRelayClient *c = as_client(t);
    if (!c || !c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (c->in_group && c->lobby.connected && !c->lobby.dead) {
        uint8_t disc[NK_LOBBY_BARE_SIZE];
        if (nk_lobby_bare(disc, NK_LOBBY_DISCONNECT) == NK_LOBBY_BARE_SIZE) {
            (void)buf_append(&c->lobby.tx, &c->lobby.tx_len, &c->lobby.tx_cap, disc, sizeof(disc),
                             NK_RELAY_CHAN_MAX_BUF);
            chan_flush_tx(&c->lobby);
        }
    }
    release_resolve_job(&c->resolve_job);
    close_all_sockets(c);
    chan_close(&c->lobby);
    c->logged_in = 0;
    c->in_group = 0;
    c->scanning = 0;
    c->resolved = 0;
    return NK_NET_OK;
}

static int rc_connect(NkAdhocTransport *t, const uint8_t group[NK_RELAY_GROUP_LEN]) {
    NkAdhocRelayClient *c = as_client(t);
    if (!c || !group) return NK_NET_BAD_ARG;
    uint8_t frame[NK_LOBBY_CONNECT_SIZE];
    if (nk_lobby_connect(frame, group) != NK_LOBBY_CONNECT_SIZE) return NK_NET_BAD_ARG;
    const int rc = queue_lobby_cmd(c, frame, sizeof(frame));
    if (rc == NK_NET_OK) c->scanning = 0;
    return rc;
}

static int rc_disconnect(NkAdhocTransport *t) {
    NkAdhocRelayClient *c = as_client(t);
    uint8_t frame[NK_LOBBY_BARE_SIZE];
    if (nk_lobby_bare(frame, NK_LOBBY_DISCONNECT) != NK_LOBBY_BARE_SIZE) return NK_NET_BAD_ARG;
    const int rc = queue_lobby_cmd(c, frame, sizeof(frame));
    c->in_group = 0;
    c->scanning = 0;
    return rc;
}

static int rc_scan(NkAdhocTransport *t) {
    NkAdhocRelayClient *c = as_client(t);
    uint8_t frame[NK_LOBBY_BARE_SIZE];
    if (nk_lobby_bare(frame, NK_LOBBY_SCAN) != NK_LOBBY_BARE_SIZE) return NK_NET_BAD_ARG;
    const int rc = queue_lobby_cmd(c, frame, sizeof(frame));
    if (rc == NK_NET_OK) c->scanning = 1;
    return rc;
}

static size_t rc_take_events(NkAdhocTransport *t, NkLobbyEvent *out, size_t max) {
    NkAdhocRelayClient *c = as_client(t);
    rc_pump(t);
    size_t n = 0u;
    while (out && n < max) {
        if (!c->ev_count) parse_lobby_rx(c);
        if (!c->ev_count) break;
        out[n++] = c->events[c->ev_head];
        c->ev_head = (c->ev_head + 1u) % NK_NET_EVENT_QUEUE;
        c->ev_count--;
    }
    return n;
}

static uint32_t rc_events_dropped(const NkAdhocTransport *t) {
    return as_const_client(t)->ev_dropped;
}

static int rc_pdp_open(NkAdhocTransport *t, uint16_t port) {
    NkAdhocRelayClient *c = as_client(t);
    if (c->shut_down) return NK_NET_DEAD;
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (port == 0u || port == 0xffffu) return NK_NET_BAD_ARG;
    int slot = -1;
    for (unsigned i = 0u; i < NK_NET_PDP_SOCKETS; i++) {
        if (c->pdp[i].used && c->pdp[i].lport == port) return NK_NET_PORT_IN_USE;
        if (!c->pdp[i].used && slot < 0) slot = (int)i;
    }
    if (slot < 0) return NK_NET_NO_SLOT;
    RelayPdpSocket *s = &c->pdp[slot];
    memset(s, 0, sizeof *s);
    chan_init(&s->chan);
    if (!open_data_chan(c, &s->chan, NK_RELAY_INIT_PDP, port, NULL, 0u)) {
        chan_close(&s->chan);
        set_error(c, "could not open PDP relay stream");
        return NK_NET_DEAD;
    }
    s->used = 1;
    s->lport = port;
    return slot;
}

static int rc_pdp_send(NkAdhocTransport *t, int handle, const uint8_t mac[NK_RELAY_MAC_LEN],
                       uint16_t port, const void *data, uint32_t len) {
    NkAdhocRelayClient *c = as_client(t);
    if (handle < 0 || (unsigned)handle >= NK_NET_PDP_SOCKETS || !c->pdp[handle].used)
        return NK_NET_NO_SOCKET;
    if (!mac || (mac[0] & 1u) || port == 0u || (len && !data)) return NK_NET_BAD_ARG;
    if (len > NK_RELAY_PDP_MAX) return NK_NET_TOO_BIG;
    RelayPdpSocket *s = &c->pdp[handle];
    chan_flush_tx(&s->chan);
    if (s->chan.dead || s->chan.remote_eof) return NK_NET_DEAD;
    uint8_t hdr[NK_RELAY_PDP_HEADER_SIZE];
    const uint16_t wire_port = nk_relay_port_out(port, c->port_offset);
    if (nk_relay_pdp_header(hdr, mac, wire_port, len) != NK_RELAY_PDP_HEADER_SIZE)
        return NK_NET_TOO_BIG;
    if (s->chan.tx_len + sizeof(hdr) + (size_t)len > NK_RELAY_CHAN_MAX_BUF) return NK_NET_AGAIN;
    if (!buf_append(&s->chan.tx, &s->chan.tx_len, &s->chan.tx_cap, hdr, sizeof(hdr),
                    NK_RELAY_CHAN_MAX_BUF) ||
        !buf_append(&s->chan.tx, &s->chan.tx_len, &s->chan.tx_cap, (const uint8_t *)data, len,
                    NK_RELAY_CHAN_MAX_BUF)) {
        s->chan.dead = 1;
        return NK_NET_DEAD;
    }
    chan_flush_tx(&s->chan);
    return s->chan.dead ? NK_NET_DEAD : NK_NET_OK;
}

static int rc_pdp_recv(NkAdhocTransport *t, int handle, uint8_t mac[NK_RELAY_MAC_LEN],
                       uint16_t *port, void *buf, uint32_t cap, uint32_t *len) {
    NkAdhocRelayClient *c = as_client(t);
    if (handle < 0 || (unsigned)handle >= NK_NET_PDP_SOCKETS || !c->pdp[handle].used)
        return NK_NET_NO_SOCKET;
    if (!mac || !port || !len || (cap && !buf)) return NK_NET_BAD_ARG;
    RelayPdpSocket *s = &c->pdp[handle];
    pump_pdp_socket(c, s);
    if (s->count == 0u) return (s->chan.dead || s->chan.remote_eof) ? NK_NET_DEAD : NK_NET_AGAIN;
    PdpDatagram *d = &s->queue[s->head];
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

static uint32_t rc_pdp_peek(NkAdhocTransport *t, int handle) {
    NkAdhocRelayClient *c = as_client(t);
    if (handle < 0 || (unsigned)handle >= NK_NET_PDP_SOCKETS || !c->pdp[handle].used) return 0u;
    RelayPdpSocket *s = &c->pdp[handle];
    pump_pdp_socket(c, s);
    return s->count ? s->queue[s->head].len : 0u;
}

static int rc_pdp_close(NkAdhocTransport *t, int handle) {
    NkAdhocRelayClient *c = as_client(t);
    if (handle < 0 || (unsigned)handle >= NK_NET_PDP_SOCKETS || !c->pdp[handle].used)
        return NK_NET_NO_SOCKET;
    close_pdp_slot(&c->pdp[handle]);
    return NK_NET_OK;
}

/* The relay client drops no datagram: a socket whose queue is full holds the rest of its stream
 * back (parse_pdp_rx), so the count stays zero. */
static uint32_t rc_pdp_dropped(const NkAdhocTransport *t) {
    (void)t;
    return 0u;
}

static int rc_ptp_listen(NkAdhocTransport *t, uint16_t sport, int backlog) {
    NkAdhocRelayClient *c = as_client(t);
    if (c->shut_down) return NK_NET_DEAD;
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (sport == 0u) sport = pick_ptp_port(c);
    if (sport == 0u) return NK_NET_NO_SLOT;
    if (ptp_listen_port_taken(c, sport)) return NK_NET_PORT_IN_USE;
    const int slot = alloc_ptp_slot(c);
    if (slot < 0) return slot;
    RelayPtpSocket *ps = &c->ptp[slot];
    ps->state = PTP_STATE_LISTEN;
    ps->lport = sport;
    ps->max_backlog =
        backlog < 1 ? 1 : (backlog > (int)NK_RELAY_PTP_BACKLOG ? (int)NK_RELAY_PTP_BACKLOG : backlog);
    if (!open_data_chan(c, &ps->chan, NK_RELAY_INIT_PTP_LISTEN, sport, NULL, 0u)) {
        close_ptp_slot(c, slot);
        set_error(c, "could not open PTP listener on relay");
        return NK_NET_DEAD;
    }
    return slot;
}

static int rc_ptp_accept(NkAdhocTransport *t, int listen_handle, uint8_t peer_mac[NK_RELAY_MAC_LEN],
                         uint16_t *peer_port) {
    NkAdhocRelayClient *c = as_client(t);
    if (listen_handle < 0 || (unsigned)listen_handle >= NK_RELAY_PTP_SOCKETS ||
        !c->ptp[listen_handle].used || c->ptp[listen_handle].state != PTP_STATE_LISTEN)
        return NK_NET_NO_SOCKET;
    rc_pump(t);
    RelayPtpSocket *ls = &c->ptp[listen_handle];
    if (ls->pending_count == 0u) {
        return (ls->chan.dead || ls->chan.remote_eof) ? NK_NET_DEAD : NK_NET_AGAIN;
    }
    const int child_handle = ls->pending_handles[0];
    for (unsigned i = 1u; i < ls->pending_count; i++) {
        ls->pending_handles[i - 1u] = ls->pending_handles[i];
    }
    ls->pending_count--;
    if (child_handle < 0 || (unsigned)child_handle >= NK_RELAY_PTP_SOCKETS ||
        !c->ptp[child_handle].used)
        return NK_NET_NO_SOCKET;
    const RelayPtpSocket *child = &c->ptp[child_handle];
    if (peer_mac) memcpy(peer_mac, child->peer_mac, NK_RELAY_MAC_LEN);
    if (peer_port) *peer_port = child->pport;
    return child_handle;
}

static int rc_ptp_open(NkAdhocTransport *t, uint16_t sport, const uint8_t dst[NK_RELAY_MAC_LEN],
                       uint16_t dport) {
    NkAdhocRelayClient *c = as_client(t);
    if (c->shut_down) return NK_NET_DEAD;
    if (!c->logged_in) return NK_NET_NOT_LOGGED_IN;
    if (!dst || dport == 0u) return NK_NET_BAD_ARG;
    if (sport == 0u) sport = pick_ptp_port(c);
    if (sport == 0u) return NK_NET_NO_SLOT;
    if (ptp_stream_taken(c, sport, dst, dport)) return NK_NET_PORT_IN_USE;
    const int slot = alloc_ptp_slot(c);
    if (slot < 0) return slot;
    RelayPtpSocket *ps = &c->ptp[slot];
    ps->state = PTP_STATE_CONNECT_WAIT_ACK;
    ps->lport = sport;
    ps->pport = dport;
    memcpy(ps->peer_mac, dst, NK_RELAY_MAC_LEN);
    if (!open_data_chan(c, &ps->chan, NK_RELAY_INIT_PTP_CONNECT, sport, dst, dport)) {
        close_ptp_slot(c, slot);
        set_error(c, "could not open PTP stream on relay");
        return NK_NET_CONNECTION_REFUSED;
    }
    return slot;
}

static int rc_ptp_connect_poll(NkAdhocTransport *t, int handle) {
    NkAdhocRelayClient *c = as_client(t);
    if (handle < 0 || (unsigned)handle >= NK_RELAY_PTP_SOCKETS || !c->ptp[handle].used)
        return NK_NET_NO_SOCKET;
    pump_ptp_socket(c, handle);
    const RelayPtpSocket *ps = &c->ptp[handle];
    if (ps->state == PTP_STATE_ESTABLISHED) return 1;
    if (ps->state == PTP_STATE_CONNECT_WAIT_ACK) return 0;
    return NK_NET_CONNECTION_REFUSED;
}

static int rc_ptp_send(NkAdhocTransport *t, int handle, const void *data, uint32_t len,
                       uint32_t *sent) {
    NkAdhocRelayClient *c = as_client(t);
    if (sent) *sent = 0u;
    if (handle < 0 || (unsigned)handle >= NK_RELAY_PTP_SOCKETS || !c->ptp[handle].used)
        return NK_NET_NO_SOCKET;
    RelayPtpSocket *ps = &c->ptp[handle];
    pump_ptp_socket(c, handle);
    if (ps->state != PTP_STATE_ESTABLISHED) {
        return ps->state == PTP_STATE_CONNECT_WAIT_ACK ? NK_NET_AGAIN : NK_NET_DEAD;
    }
    if (ps->chan.dead || ps->chan.remote_eof) return NK_NET_DEAD;
    if (len == 0u) return NK_NET_OK;
    if (!data) return NK_NET_BAD_ARG;
    const size_t avail = ps->chan.tx_len + NK_RELAY_PTP_DATA_HEADER_SIZE < NK_RELAY_CHAN_MAX_BUF
                             ? NK_RELAY_CHAN_MAX_BUF - ps->chan.tx_len - NK_RELAY_PTP_DATA_HEADER_SIZE
                             : 0u;
    if (avail == 0u) return NK_NET_AGAIN;
    uint32_t chunk = len > NK_RELAY_PTP_MAX ? NK_RELAY_PTP_MAX : len;
    if ((size_t)chunk > avail) chunk = (uint32_t)avail;
    uint8_t hdr[NK_RELAY_PTP_DATA_HEADER_SIZE];
    if (nk_relay_ptp_data_header(hdr, chunk) != NK_RELAY_PTP_DATA_HEADER_SIZE)
        return NK_NET_TOO_BIG;
    if (!buf_append(&ps->chan.tx, &ps->chan.tx_len, &ps->chan.tx_cap, hdr, sizeof(hdr),
                    NK_RELAY_CHAN_MAX_BUF) ||
        !buf_append(&ps->chan.tx, &ps->chan.tx_len, &ps->chan.tx_cap, (const uint8_t *)data, chunk,
                    NK_RELAY_CHAN_MAX_BUF)) {
        ps->chan.dead = 1;
        return NK_NET_DEAD;
    }
    chan_flush_tx(&ps->chan);
    if (ps->chan.dead) return NK_NET_DEAD;
    if (sent) *sent = chunk;
    return NK_NET_OK;
}

static int rc_ptp_recv(NkAdhocTransport *t, int handle, void *buf, uint32_t cap, uint32_t *len) {
    NkAdhocRelayClient *c = as_client(t);
    if (len) *len = 0u;
    if (handle < 0 || (unsigned)handle >= NK_RELAY_PTP_SOCKETS || !c->ptp[handle].used)
        return NK_NET_NO_SOCKET;
    if (!len || (cap && !buf)) return NK_NET_BAD_ARG;
    RelayPtpSocket *ps = &c->ptp[handle];
    pump_ptp_socket(c, handle);
    if (ps->app_rx_len == 0u) {
        if (ps->state == PTP_STATE_REFUSED || ps->chan.dead || ps->chan.remote_eof)
            return NK_NET_DEAD;
        return NK_NET_AGAIN;
    }
    if (cap == 0u) return NK_NET_OK;
    const size_t take = ps->app_rx_len < (size_t)cap ? ps->app_rx_len : (size_t)cap;
    memcpy(buf, ps->app_rx, take);
    if (take < ps->app_rx_len) memmove(ps->app_rx, ps->app_rx + take, ps->app_rx_len - take);
    ps->app_rx_len -= take;
    *len = (uint32_t)take;
    return NK_NET_OK;
}

static uint32_t rc_ptp_peek(NkAdhocTransport *t, int handle) {
    NkAdhocRelayClient *c = as_client(t);
    if (handle < 0 || (unsigned)handle >= NK_RELAY_PTP_SOCKETS || !c->ptp[handle].used) return 0u;
    pump_ptp_socket(c, handle);
    return (uint32_t)c->ptp[handle].app_rx_len;
}

static bool rc_ptp_is_dead(const NkAdhocTransport *t, int handle) {
    const NkAdhocRelayClient *c = as_const_client(t);
    if (handle < 0 || (unsigned)handle >= NK_RELAY_PTP_SOCKETS || !c->ptp[handle].used)
        return true;
    const RelayPtpSocket *ps = &c->ptp[handle];
    return ps->state == PTP_STATE_REFUSED || ps->chan.dead || ps->chan.remote_eof;
}

static bool rc_ptp_accept_pending(const NkAdhocTransport *t, int listen_handle) {
    const NkAdhocRelayClient *c = as_const_client(t);
    if (listen_handle < 0 || (unsigned)listen_handle >= NK_RELAY_PTP_SOCKETS) return false;
    const RelayPtpSocket *ls = &c->ptp[listen_handle];
    return ls->used && ls->state == PTP_STATE_LISTEN && ls->pending_count > 0u;
}

static int rc_ptp_close(NkAdhocTransport *t, int handle) {
    NkAdhocRelayClient *c = as_client(t);
    if (handle < 0 || (unsigned)handle >= NK_RELAY_PTP_SOCKETS || !c->ptp[handle].used)
        return NK_NET_NO_SOCKET;
    close_ptp_slot(c, handle);
    return NK_NET_OK;
}

static const NkAdhocTransportOps s_relay_ops = {
    "relay",
    rc_pump,
    rc_login, rc_logout, rc_connect, rc_disconnect, rc_scan, rc_take_events, rc_events_dropped,
    rc_pdp_open, rc_pdp_send, rc_pdp_recv, rc_pdp_peek, rc_pdp_close, rc_pdp_dropped,
    rc_ptp_listen, rc_ptp_accept_pending, rc_ptp_accept, rc_ptp_open, rc_ptp_connect_poll,
    rc_ptp_send, rc_ptp_recv, rc_ptp_peek, rc_ptp_is_dead, rc_ptp_close,
};

NkAdhocRelayClient *nk_relay_client_create(const NkAdhocRelayConfig *cfg) {
    if (!cfg || !cfg->host || !cfg->host[0] || strlen(cfg->host) > NK_ADHOC_SERVER_MAX_LEN)
        return NULL;
    NkAdhocRelayClient *c = calloc(1, sizeof *c);
    if (!c) return NULL;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        free(c);
        return NULL;
    }
    c->wsa_started = 1;
#endif
    c->base.ops = &s_relay_ops;
    snprintf(c->host, sizeof(c->host), "%s", cfg->host);
    c->lobby_port = cfg->lobby_port ? cfg->lobby_port : NK_RELAY_LOBBY_PORT;
    c->data_port = cfg->data_port ? cfg->data_port : NK_RELAY_DATA_PORT;
    c->port_offset = cfg->port_offset;
    chan_init(&c->lobby);
    for (unsigned i = 0u; i < NK_NET_PDP_SOCKETS; i++) chan_init(&c->pdp[i].chan);
    for (unsigned i = 0u; i < NK_RELAY_PTP_SOCKETS; i++) {
        chan_init(&c->ptp[i].chan);
        c->ptp[i].parent_listen = -1;
    }
    return c;
}

void nk_relay_client_destroy(NkAdhocRelayClient *client) {
    if (!client) return;
    if (client->logged_in) (void)rc_logout(&client->base);
    release_resolve_job(&client->resolve_job);
    close_all_sockets(client);
    chan_close(&client->lobby);
#ifdef _WIN32
    if (client->wsa_started) WSACleanup();
#endif
    free(client);
}

NkAdhocTransport *nk_relay_client_transport(NkAdhocRelayClient *client) {
    return client ? &client->base : NULL;
}

bool nk_relay_client_lobby_connected(const NkAdhocRelayClient *client) {
    return client && client->logged_in && client->lobby.connected && !client->lobby.dead &&
           !client->lobby.remote_eof;
}

bool nk_relay_client_lobby_dead(const NkAdhocRelayClient *client) {
    return !client || client->lobby.dead || client->lobby.remote_eof;
}

void nk_relay_client_shutdown(NkAdhocRelayClient *client, const char *reason) {
    if (!client) return;
    release_resolve_job(&client->resolve_job);
    close_all_sockets(client);
    chan_close(&client->lobby);
    client->lobby.dead = 1;
    client->logged_in = 0;
    client->in_group = 0;
    client->scanning = 0;
    client->shut_down = 1;
    set_error(client, (reason && reason[0]) ? reason : "shut down");
}

bool nk_relay_client_is_shut_down(const NkAdhocRelayClient *client) {
    return client && client->shut_down;
}

const char *nk_relay_client_last_error(const NkAdhocRelayClient *client) {
    if (!client) return "no client";
    return client->last_error;
}
