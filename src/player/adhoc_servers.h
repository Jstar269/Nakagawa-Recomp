/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#ifndef NK_PLAYER_ADHOC_SERVERS_H
#define NK_PLAYER_ADHOC_SERVERS_H

/* The ad hoc server list for Online play (#1139; owner decisions of 2026-11-19).
 *
 * The server picker shows the shipped servers (nk_adhoc_server_preset), then PPSSPP's public list
 * of ad hoc servers. That list is downloaded over HTTPS from NK_ADHOC_LIST_URL only after the user
 * turned Online play on (chose TURN ON on the Online play card), and only when the user opens the
 * picker or chooses Refresh in it: never at start, never in the background, never while Online
 * play is off. The download goes through the player's HTTPS transport (package_builder.h, WinHTTP
 * on Windows, which checks the certificate against the system's trust store); where the player has
 * no HTTPS transport the live list is unavailable by name. When the live list is unavailable, the
 * copy shipped with the player is shown instead (nk_adhoc_list_load_shipped). The downloaded list
 * is kept for the player session only, never written to disk.
 *
 * The list is untrusted input. It is bounded (NK_ADHOC_LIST_MAX_BYTES, NK_ADHOC_LIST_MAX_ENTRIES),
 * parsed with the project's JSON reader, and each entry is checked: the host by
 * nk_adhoc_relay_host_ok, the name and place for length, control characters and bidirectional
 * overrides. Only servers whose data_mode is "AemuPostoffice" are kept: in PPSSPP (Core/HLE/
 * sceNetAdhoc.cpp at 3bc60270eae3069f13dfbfdb7292a7e1375de6f6) that value alone turns on the
 * aemu_postoffice data relay, and the relay is the only protocol this client speaks (a P2P server
 * runs the lobby only). This reading of data_mode is PPSSPP-derived, not PSP-measured. Entries marked
 * "hidden" are skipped, as PPSSPP's picker does. Every entry dropped is named in one log line. No
 * other field is read: the icon, web page, status page and Discord links are never fetched or shown.
 * An entry never connects by itself: picking one makes it the selected server, and the Online play
 * card names its exact host before the first connection to it. */

#include <stdbool.h>
#include <stddef.h>

#include "nk_adhoc_relay.h"
#include "package_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NK_ADHOC_LIST_HOST "metadata.ppsspp.org"
#define NK_ADHOC_LIST_URL "https://" NK_ADHOC_LIST_HOST "/adhoc-servers.json"
#define NK_ADHOC_LIST_USER_AGENT "NakagawaRecomp/OnlinePlay"
#define NK_ADHOC_LIST_MAX_BYTES (64u * 1024u)
#define NK_ADHOC_LIST_MAX_ENTRIES 64u
#define NK_ADHOC_LIST_NAME_MAX 64u
#define NK_ADHOC_LIST_LOCATION_MAX 48u

typedef struct NkAdhocListEntry {
    char host[NK_ADHOC_SERVER_MAX_LEN + 1u];
    char name[NK_ADHOC_LIST_NAME_MAX + 1u];
    char location[NK_ADHOC_LIST_LOCATION_MAX + 1u];
} NkAdhocListEntry;

typedef struct NkAdhocServerList {
    NkAdhocListEntry entries[NK_ADHOC_LIST_MAX_ENTRIES];
    size_t count;
    size_t dropped; /* entries refused or past the cap, each named in the log */
} NkAdhocServerList;

typedef enum NkAdhocListStatus {
    NK_ADHOC_LIST_OK = 0,
    NK_ADHOC_LIST_OFF,              /* Online play is off: nothing was fetched */
    NK_ADHOC_LIST_UNSUPPORTED,      /* this build has no HTTPS transport */
    NK_ADHOC_LIST_UNREACHABLE,      /* the request failed or did not answer 200 */
    NK_ADHOC_LIST_REDIRECT_REFUSED, /* the answer came from another host */
    NK_ADHOC_LIST_TOO_LARGE,
    NK_ADHOC_LIST_MALFORMED,        /* not the list's JSON form */
    NK_ADHOC_LIST_EMPTY,            /* no entry passed the checks */
    NK_ADHOC_LIST_NO_MEMORY
} NkAdhocListStatus;

/* One line of the log: an entry dropped and why, or the list refused and why. */
typedef void (*NkAdhocListLogFn)(void *context, const char *line);

/* A short sentence for the picker: "Live list unavailable: ..." and so on. */
const char *nk_adhoc_list_status_text(NkAdhocListStatus status);

/* Reads a list in PPSSPP's form ({"servers": [ ... ]}) from text[0..len). Fills out with the
 * entries that pass the checks, in their order, and logs each one dropped. Returns OK, MALFORMED,
 * TOO_LARGE, EMPTY or NO_MEMORY; out is always left consistent (count 0 on refusal). */
NkAdhocListStatus nk_adhoc_list_parse(const char *text, size_t len, NkAdhocServerList *out,
                                      NkAdhocListLogFn log, void *log_context);

/* The copy shipped with the player: the relay entries of PPSSPP's assets/adhoc-servers.json at
 * 3bc60270eae3069f13dfbfdb7292a7e1375de6f6 (name, host, location and data_mode only), read
 * through nk_adhoc_list_parse. */
NkAdhocListStatus nk_adhoc_list_load_shipped(NkAdhocServerList *out, NkAdhocListLogFn log,
                                             void *log_context);

/* Downloads and reads the live list. online_play_on is the user's Online play setting: false
 * returns NK_ADHOC_LIST_OFF before any transport is touched. A NULL transport selects the player's
 * native HTTPS transport; tests pass a fake one. Blocks: call it from a worker thread. */
NkAdhocListStatus nk_adhoc_list_fetch(PackageHttpOpen transport, void *transport_context,
                                      bool online_play_on, NkAdhocServerList *out,
                                      NkAdhocListLogFn log, void *log_context);

/* The console ID Online play sends (#1139; owner decision of 2026-11-19): the random WLAN MAC
 * address the runtime made for this installation the first time a game asked for it, kept in the
 * registry overlay <user data root>/registry/system.json ("console": {"wlan_mac_hex": ...}). It
 * is never the PC's hardware address. "New console ID" replaces only that value with a new random
 * one (first octet's two low bits clear, as the runtime makes it) and leaves every other byte of
 * the file as it was, the OpenPSID included. The file is untrusted: it is bounded (1 MiB, the
 * runtime's own limit), read with the project's JSON reader, and changed only when it holds one
 * wlan_mac_hex in the form the runtime writes; the new file is written beside it and renamed over
 * it. The player refuses the change while a game runs, since the runtime keeps the file's values
 * in memory and writes them back. */
#define NK_CONSOLE_ID_HEX_LEN 12u

typedef enum NkConsoleIdStatus {
    NK_CONSOLE_ID_OK = 0,
    NK_CONSOLE_ID_NONE,        /* no file or no console ID yet: made the first time a game asks */
    NK_CONSOLE_ID_UNREADABLE,  /* the file exists but cannot be read, or is over 1 MiB */
    NK_CONSOLE_ID_UNEXPECTED,  /* not the form the runtime writes: left as it is */
    NK_CONSOLE_ID_NO_RANDOM,   /* the system's random source failed */
    NK_CONSOLE_ID_WRITE_FAILED /* the new file could not be written; the old one stays */
} NkConsoleIdStatus;

const char *nk_adhoc_console_id_status_text(NkConsoleIdStatus status);
/* <root><sep>registry<sep>system.json; false when it does not fit. */
bool nk_adhoc_console_registry_path(const char *user_data_root, char *out, size_t out_size);
/* Reads the console ID as 12 lower-case hex digits. */
NkConsoleIdStatus nk_adhoc_console_id_read(const char *registry_path,
                                           char out_hex[NK_CONSOLE_ID_HEX_LEN + 1u]);
/* Replaces it with a new random one, returned in new_hex. */
NkConsoleIdStatus nk_adhoc_console_id_renew(const char *registry_path,
                                            char new_hex[NK_CONSOLE_ID_HEX_LEN + 1u]);
/* "02:1a:2b:3c:4d:5e" from 12 hex digits; false for anything else. */
bool nk_adhoc_console_id_display(const char *hex, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NK_PLAYER_ADHOC_SERVERS_H */
