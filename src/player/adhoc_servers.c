/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/* The Online play server list (#1139): adhoc_servers.h says what it reads, from where and when. */

#include "adhoc_servers.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_json.h"

#define NK_ADHOC_LIST_READ_CHUNK 4096u

/* The relay entries of PPSSPP's assets/adhoc-servers.json at
 * 3bc60270eae3069f13dfbfdb7292a7e1375de6f6 (all 22 are "AemuPostoffice"; none is hidden), with
 * only the four fields this player reads. Non-ASCII place names are JSON escapes. */
static const char s_shipped_json[] =
    "{\"servers\":[\n"
    "{\"name\":\"Socom Adhoc Server\",\"host\":\"socom.cc\",\"location\":\"France\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Madness Gaming Network\",\"host\":\"psp.mgn.pub\",\"location\":\"Alaska USA\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"EA Nation Hub\",\"host\":\"eahub.eu\",\"location\":\"France\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Psi-Hate\",\"host\":\"psi-hate.com\",\"location\":\"Minnesota USA\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Relay Brasileiro\",\"host\":\"jpa36a7.glddns.com\",\"location\":\"S\\u00e3o Paulo Brazil\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"ArenaAnywhere SA\",\"host\":\"relay-sa.arenaanywhere.site\",\"location\":\"South Africa\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"ArenaAnywhere EU\",\"host\":\"relay.arenaanywhere.site\",\"location\":\"Europe\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"ArenaAnywhere US\",\"host\":\"relay-usa.arenaanywhere.site\",\"location\":\"US\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"ArenaAnywhere Asia\",\"host\":\"relay-asia.arenaanywhere.site\",\"location\":\"Singapore\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Saya Akan Lawan! (beta)\",\"host\":\"sayaakanlawan.my.id\",\"location\":\"Indonesia\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Games Nexus\",\"host\":\"adhoc.gamesnexus.ovh\",\"location\":\"Milan Italy\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"CVN-Play\",\"host\":\"cvn-play.top\",\"location\":\"China\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"NT Arena Brasil\",\"host\":\"ntarena.duckdns.org\",\"location\":\"Brazil\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Lason Club Manila Server\",\"host\":\"38.54.80.95\",\"location\":\"Manila, Philippines\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Neko PSP\",\"host\":\"cat.wjg.jp\",\"location\":\"Japan\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Sprawl Relay - Brazil\",\"host\":\"brazil-01.sprawl-relay.link\",\"location\":\"Brazil\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"PSP Brasil\",\"host\":\"pspbrasil.duckdns.org\",\"location\":\"S\\u00e3o Paulo, Brazil\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Jeddah Relay - Middle East\",\"host\":\"jeddah-relay.ddns.net\",\"location\":\"Jeddah, Saudi Arabia\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Alpha Adhoc Server\",\"host\":\"140.245.237.12\",\"location\":\"India\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"aemu_postoffice test server\",\"host\":\"185.183.157.234\",\"location\":\"Germany\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"Andexplaystream\",\"host\":\"psp.andexplaystream.online\",\"location\":\"Jakarta, Indonesia\",\"data_mode\":\"AemuPostoffice\"},\n"
    "{\"name\":\"SWAENET\",\"host\":\"132.226.189.224\",\"location\":\"Mumbai,India\",\"data_mode\":\"AemuPostoffice\"}\n"
    "]}\n";

const char *nk_adhoc_list_shipped_json(void) { return s_shipped_json; }

const char *nk_adhoc_list_status_text(NkAdhocListStatus status) {
    switch (status) {
    case NK_ADHOC_LIST_OK: return "Live list from PPSSPP.";
    case NK_ADHOC_LIST_OFF: return "Live list not fetched: Online play is off.";
    case NK_ADHOC_LIST_UNSUPPORTED:
        return "Live list unavailable: this build has no secure web connection.";
    case NK_ADHOC_LIST_UNREACHABLE: return "Live list unavailable: the list server did not answer.";
    case NK_ADHOC_LIST_REDIRECT_REFUSED:
        return "Live list refused: it was sent from another host.";
    case NK_ADHOC_LIST_TOO_LARGE: return "Live list refused: it is larger than 64 KiB.";
    case NK_ADHOC_LIST_MALFORMED: return "Live list refused: it is not in the expected form.";
    case NK_ADHOC_LIST_EMPTY: return "Live list refused: no server in it passed the checks.";
    case NK_ADHOC_LIST_NO_MEMORY: return "Live list unavailable: out of memory.";
    }
    return "Live list unavailable.";
}

static void list_log(NkAdhocListLogFn log, void *context, const char *fmt, ...) {
    char line[256];
    va_list args;
    if (!log) return;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    log(context, line);
}

/* A name or place the picker may show: at most max bytes, and no C0 or C1 control, DEL, or
   bidirectional mark or override (U+061C, U+200E, U+200F, U+202A-U+202E, U+2066-U+2069), which
   could make it read differently from its bytes. The parser has already checked the UTF-8. */
static const char *display_text_problem(const char *text, size_t max) {
    const unsigned char *s = (const unsigned char *)text;
    size_t len = strlen(text);
    if (len > max) return "is too long";
    for (size_t i = 0; i < len; i++) {
        if (s[i] < 0x20u || s[i] == 0x7fu) return "holds a control character";
        if (s[i] == 0xc2u && i + 1u < len && s[i + 1u] >= 0x80u && s[i + 1u] <= 0x9fu)
            return "holds a control character";
        if (s[i] == 0xd8u && i + 1u < len && s[i + 1u] == 0x9cu)
            return "holds a bidirectional mark";
        if (s[i] == 0xe2u && i + 2u < len) {
            const unsigned char b1 = s[i + 1u], b2 = s[i + 2u];
            if ((b1 == 0x80u && (b2 == 0x8eu || b2 == 0x8fu || (b2 >= 0xaau && b2 <= 0xaeu))) ||
                (b1 == 0x81u && b2 >= 0xa6u && b2 <= 0xa9u))
                return "holds a bidirectional mark";
        }
    }
    return NULL;
}

/* An optional string member: absent is "", any other type is a problem. */
static bool optional_string(const NkJsonNode *entry, const char *key, const char **out) {
    const NkJsonNode *node = nk_json_obj_get(entry, key);
    *out = "";
    if (!node) return true;
    if (!nk_json_is_string(node)) return false;
    *out = nk_json_get_string(node);
    return *out != NULL;
}

NkAdhocListStatus nk_adhoc_list_parse(const char *text, size_t len, NkAdhocServerList *out,
                                      NkAdhocListLogFn log, void *log_context) {
    char err[128];
    NkJsonNode *root;
    const NkJsonNode *servers;
    size_t total;
    if (!out) return NK_ADHOC_LIST_MALFORMED;
    memset(out, 0, sizeof(*out));
    if (!text) return NK_ADHOC_LIST_MALFORMED;
    if (len > NK_ADHOC_LIST_MAX_BYTES) {
        list_log(log, log_context, "online play: server list refused: %zu bytes, over the %u-byte cap",
                 len, NK_ADHOC_LIST_MAX_BYTES);
        return NK_ADHOC_LIST_TOO_LARGE;
    }
    if (!nk_json_validate_utf8((const uint8_t *)text, len)) {
        list_log(log, log_context, "online play: server list refused: it is not UTF-8");
        return NK_ADHOC_LIST_MALFORMED;
    }
    /* A string holding \u0000 would end early in a C string, so it could read as something it
       is not; the whole list is refused instead. Each byte is inside the checked span. */
    for (size_t i = 0; i + 6u <= len; i++) {
        if (text[i] == '\\' && memcmp(text + i + 1u, "u0000", 5u) == 0) {
            list_log(log, log_context, "online play: server list refused: a string holds \\u0000");
            return NK_ADHOC_LIST_MALFORMED;
        }
    }
    err[0] = '\0';
    root = nk_json_parse(text, len, err, sizeof(err));
    if (!root) {
        list_log(log, log_context, "online play: server list refused: %s",
                 err[0] ? err : "not JSON");
        return NK_ADHOC_LIST_MALFORMED;
    }
    servers = nk_json_is_object(root) ? nk_json_obj_get(root, "servers") : NULL;
    if (!servers || !nk_json_is_array(servers)) {
        list_log(log, log_context, "online play: server list refused: no \"servers\" array");
        nk_json_free(root);
        return NK_ADHOC_LIST_MALFORMED;
    }
    total = nk_json_array_count(servers);
    for (size_t i = 0; i < total; i++) {
        const NkJsonNode *entry = nk_json_array_get(servers, i);
        const NkJsonNode *host_node, *hidden_node;
        const char *name = "", *location = "", *mode = "", *problem = NULL;
        bool hidden = false;
        if (out->count >= NK_ADHOC_LIST_MAX_ENTRIES) {
            out->dropped += total - i;
            list_log(log, log_context,
                     "online play: server list entries %zu to %zu dropped: past the %u-entry cap",
                     i, total - 1u, NK_ADHOC_LIST_MAX_ENTRIES);
            break;
        }
        if (!entry || !nk_json_is_object(entry)) {
            problem = "is not an object";
        } else if (!(host_node = nk_json_obj_get(entry, "host")) || !nk_json_is_string(host_node) ||
                   !nk_adhoc_relay_host_ok(nk_json_get_string(host_node))) {
            problem = "has no valid host name";
        } else if (!optional_string(entry, "name", &name) ||
                   !optional_string(entry, "location", &location) ||
                   !optional_string(entry, "data_mode", &mode)) {
            problem = "has a name, location or data_mode that is not a string";
        } else if ((hidden_node = nk_json_obj_get(entry, "hidden")) != NULL &&
                   !nk_json_get_bool(hidden_node, &hidden)) {
            problem = "has a hidden flag that is not true or false";
        } else if (hidden) {
            problem = "is marked hidden";
        } else if (strcmp(mode, "AemuPostoffice") != 0) {
            problem = "is a lobby-only (P2P) server; this player speaks the relay only";
        } else {
            const char *name_problem = display_text_problem(name, NK_ADHOC_LIST_NAME_MAX);
            const char *place_problem = display_text_problem(location, NK_ADHOC_LIST_LOCATION_MAX);
            if (name_problem) problem = name_problem;
            else if (place_problem) problem = place_problem;
        }
        if (!problem) {
            const char *host = nk_json_get_string(host_node);
            for (size_t j = 0; j < out->count; j++) {
                if (strcmp(out->entries[j].host, host) == 0) problem = "repeats an earlier host";
            }
            if (!problem) {
                NkAdhocListEntry *e = &out->entries[out->count++];
                snprintf(e->host, sizeof(e->host), "%s", host);
                snprintf(e->name, sizeof(e->name), "%s", name[0] ? name : host);
                snprintf(e->location, sizeof(e->location), "%s", location);
                continue;
            }
        }
        out->dropped++;
        list_log(log, log_context, "online play: server list entry %zu dropped: it %s", i, problem);
    }
    nk_json_free(root);
    if (out->count == 0u) {
        list_log(log, log_context, "online play: server list refused: no entry passed the checks");
        return NK_ADHOC_LIST_EMPTY;
    }
    return NK_ADHOC_LIST_OK;
}

NkAdhocListStatus nk_adhoc_list_load_shipped(NkAdhocServerList *out, NkAdhocListLogFn log,
                                             void *log_context) {
    return nk_adhoc_list_parse(s_shipped_json, sizeof(s_shipped_json) - 1u, out, log, log_context);
}

NkAdhocListStatus nk_adhoc_list_fetch(PackageHttpOpen transport, void *transport_context,
                                      bool online_play_on, NkAdhocServerList *out,
                                      NkAdhocListLogFn log, void *log_context) {
    static const PackageHttpOptions options = { NK_ADHOC_LIST_USER_AGENT };
    PackageHttpResponse response;
    char *buffer = NULL;
    size_t used = 0u;
    NkAdhocListStatus status;
    if (out) memset(out, 0, sizeof(*out));
    if (!out) return NK_ADHOC_LIST_MALFORMED;
    if (!online_play_on) return NK_ADHOC_LIST_OFF;
    if (!transport) {
        transport = package_builder_native_transport();
        transport_context = (void *)&options;
    }
    if (!transport) {
        list_log(log, log_context, "online play: live server list unavailable: no HTTPS transport "
                                   "in this build; the shipped list is used");
        return NK_ADHOC_LIST_UNSUPPORTED;
    }
    memset(&response, 0, sizeof(response));
    if (!transport(transport_context, NK_ADHOC_LIST_URL, NK_ADHOC_LIST_HOST, &response)) {
        list_log(log, log_context, "online play: live server list unavailable: %s did not answer",
                 NK_ADHOC_LIST_HOST);
        return NK_ADHOC_LIST_UNREACHABLE;
    }
    /* The bytes count only if they came from the list host, whatever redirects were followed. */
    if (!response.final_url || !package_builder_url_allowed(response.final_url, NK_ADHOC_LIST_HOST)) {
        if (response.close) response.close(response.context);
        list_log(log, log_context, "online play: live server list refused: it was sent from "
                                   "another host");
        return NK_ADHOC_LIST_REDIRECT_REFUSED;
    }
    if (response.has_content_length && response.content_length > NK_ADHOC_LIST_MAX_BYTES) {
        if (response.close) response.close(response.context);
        list_log(log, log_context, "online play: live server list refused: %llu bytes announced, "
                                   "over the %u-byte cap",
                 (unsigned long long)response.content_length, NK_ADHOC_LIST_MAX_BYTES);
        return NK_ADHOC_LIST_TOO_LARGE;
    }
    buffer = (char *)malloc(NK_ADHOC_LIST_MAX_BYTES + 1u);
    if (!buffer) {
        if (response.close) response.close(response.context);
        return NK_ADHOC_LIST_NO_MEMORY;
    }
    status = NK_ADHOC_LIST_OK;
    for (;;) {
        unsigned char chunk[NK_ADHOC_LIST_READ_CHUNK];
        size_t got = 0u;
        if (!response.read || !response.read(response.context, chunk, sizeof(chunk), &got)) {
            list_log(log, log_context, "online play: live server list unavailable: the download "
                                       "from %s failed", NK_ADHOC_LIST_HOST);
            status = NK_ADHOC_LIST_UNREACHABLE;
            break;
        }
        if (got == 0u) break;
        if (got > sizeof(chunk) || got > NK_ADHOC_LIST_MAX_BYTES - used) {
            list_log(log, log_context, "online play: live server list refused: over the %u-byte cap",
                     NK_ADHOC_LIST_MAX_BYTES);
            status = NK_ADHOC_LIST_TOO_LARGE;
            break;
        }
        memcpy(buffer + used, chunk, got);
        used += got;
    }
    if (response.close) response.close(response.context);
    if (status == NK_ADHOC_LIST_OK) {
        buffer[used] = '\0';
        status = nk_adhoc_list_parse(buffer, used, out, log, log_context);
    }
    free(buffer);
    return status;
}
