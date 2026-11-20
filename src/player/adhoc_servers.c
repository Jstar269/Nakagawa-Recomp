/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/* The Online play server list and console ID (#1139): adhoc_servers.h says what each reads,
   from where and when. */

#if defined(_WIN32) || defined(_WIN64)
#define _CRT_RAND_S /* rand_s, the system's random source (RtlGenRandom), as the runtime uses */
#endif

#include "adhoc_servers.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "nk_json.h"
#include "nk_platform.h"

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

/* ---- console ID ---------------------------------------------------------------------------- */

#define NK_CONSOLE_REGISTRY_MAX_BYTES (1024u * 1024u) /* the runtime's SREG_OVERLAY_MAX_BYTES */

static const char k_hex_digits[] = "0123456789abcdef";

const char *nk_adhoc_console_id_status_text(NkConsoleIdStatus status) {
    switch (status) {
    case NK_CONSOLE_ID_OK: return "New console ID made.";
    case NK_CONSOLE_ID_NONE:
        return "No console ID yet: it is made the first time a game asks for it.";
    case NK_CONSOLE_ID_UNREADABLE:
        return "Console ID not changed: the console profile file cannot be read.";
    case NK_CONSOLE_ID_UNEXPECTED:
        return "Console ID not changed: the console profile file is not in the expected form.";
    case NK_CONSOLE_ID_NO_RANDOM:
        return "Console ID not changed: the system's random source failed.";
    case NK_CONSOLE_ID_WRITE_FAILED:
        return "Console ID not changed: the new console profile file could not be written.";
    }
    return "Console ID not changed.";
}

bool nk_adhoc_console_registry_path(const char *user_data_root, char *out, size_t out_size) {
    const char sep = nk_platform_path_separator();
    int w;
    if (!out || out_size == 0u) return false;
    out[0] = '\0';
    if (!user_data_root || !user_data_root[0]) return false;
    w = snprintf(out, out_size, "%s%cregistry%csystem.json", user_data_root, sep, sep);
    if (w <= 0 || (size_t)w >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 12 hex digits whose first octet has its two low bits clear (the runtime's rule). */
static bool console_mac_hex_ok(const char *hex) {
    for (size_t i = 0; i < NK_CONSOLE_ID_HEX_LEN; i++)
        if (hex_value(hex[i]) < 0) return false;
    return hex[NK_CONSOLE_ID_HEX_LEN] == '\0' && (hex_value(hex[1]) & 0x3) == 0;
}

static bool console_psid_hex_ok(const char *hex) {
    bool nonzero = false;
    for (size_t i = 0; i < 32u; i++) {
        const int v = hex_value(hex[i]);
        if (v < 0) return false;
        nonzero = nonzero || v != 0;
    }
    return hex[32] == '\0' && nonzero;
}

/* Reads the file (bounded) into a NUL-terminated buffer. */
static NkConsoleIdStatus console_file_read(const char *path, char **out, size_t *len_out) {
    FILE *f;
    char *buf;
    size_t got;
    int failed;
    *out = NULL;
    *len_out = 0u;
    if (!path || !path[0] || !nk_platform_file_exists(path)) return NK_CONSOLE_ID_NONE;
    f = nk_fopen_utf8(path, "rb");
    if (!f) return NK_CONSOLE_ID_UNREADABLE;
    buf = (char *)malloc((size_t)NK_CONSOLE_REGISTRY_MAX_BYTES + 1u);
    if (!buf) {
        fclose(f);
        return NK_CONSOLE_ID_UNREADABLE;
    }
    got = fread(buf, 1u, (size_t)NK_CONSOLE_REGISTRY_MAX_BYTES + 1u, f);
    failed = ferror(f);
    fclose(f);
    if (failed || got > NK_CONSOLE_REGISTRY_MAX_BYTES) {
        free(buf);
        return NK_CONSOLE_ID_UNREADABLE;
    }
    buf[got] = '\0';
    *out = buf;
    *len_out = got;
    return NK_CONSOLE_ID_OK;
}

/* The console ID the text holds, checked as the runtime checks it: NONE when the file has no
   "console" object yet (the runtime adds one the first time a game asks), UNEXPECTED for any
   other shape. */
static NkConsoleIdStatus console_id_from_text(const char *text, size_t len,
                                              char out_hex[NK_CONSOLE_ID_HEX_LEN + 1u]) {
    char err[96];
    NkJsonNode *root;
    const NkJsonNode *console;
    NkConsoleIdStatus status = NK_CONSOLE_ID_UNEXPECTED;
    out_hex[0] = '\0';
    if (memchr(text, '\0', len) != NULL || !nk_json_validate_utf8((const uint8_t *)text, len))
        return NK_CONSOLE_ID_UNEXPECTED;
    root = nk_json_parse(text, len, err, sizeof(err));
    if (!root) return NK_CONSOLE_ID_UNEXPECTED;
    console = nk_json_is_object(root) ? nk_json_obj_get(root, "console") : NULL;
    if (nk_json_is_object(root) && !console) {
        status = NK_CONSOLE_ID_NONE;
    } else if (console && nk_json_is_object(console)) {
        const NkJsonNode *mac = nk_json_obj_get(console, "wlan_mac_hex");
        const NkJsonNode *psid = nk_json_obj_get(console, "open_psid_hex");
        const char *mac_text = mac && nk_json_is_string(mac) ? nk_json_get_string(mac) : NULL;
        const char *psid_text = psid && nk_json_is_string(psid) ? nk_json_get_string(psid) : NULL;
        if (mac_text && psid_text && strlen(mac_text) == NK_CONSOLE_ID_HEX_LEN &&
            console_mac_hex_ok(mac_text) && strlen(psid_text) == 32u &&
            console_psid_hex_ok(psid_text)) {
            for (size_t i = 0; i < NK_CONSOLE_ID_HEX_LEN; i++)
                out_hex[i] = k_hex_digits[hex_value(mac_text[i])];
            out_hex[NK_CONSOLE_ID_HEX_LEN] = '\0';
            status = NK_CONSOLE_ID_OK;
        }
    }
    nk_json_free(root);
    return status;
}

NkConsoleIdStatus nk_adhoc_console_id_read(const char *registry_path,
                                           char out_hex[NK_CONSOLE_ID_HEX_LEN + 1u]) {
    char *text = NULL;
    size_t len = 0u;
    NkConsoleIdStatus status;
    if (!out_hex) return NK_CONSOLE_ID_UNEXPECTED;
    out_hex[0] = '\0';
    status = console_file_read(registry_path, &text, &len);
    if (status != NK_CONSOLE_ID_OK) return status;
    status = console_id_from_text(text, len, out_hex);
    free(text);
    return status;
}

static bool console_random_bytes(uint8_t *out, size_t n) {
#if defined(_WIN32) || defined(_WIN64)
    for (size_t i = 0; i < n; i++) {
        unsigned int r = 0;
        if (rand_s(&r) != 0) return false;
        out[i] = (uint8_t)r;
    }
    return true;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return false;
    got = fread(out, 1u, n, f);
    fclose(f);
    return got == n;
#endif
}

/* Writes data beside path and renames it over path, as the runtime writes the file. */
static bool console_file_replace(const char *path, const char *data, size_t len) {
    const size_t plen = strlen(path);
    char *tmp = (char *)malloc(plen + sizeof(".new"));
    FILE *f;
    bool ok;
    if (!tmp) return false;
    snprintf(tmp, plen + sizeof(".new"), "%s.new", path);
    f = nk_platform_fopen_private(tmp, "wb");
    ok = f != NULL && fwrite(data, 1u, len, f) == len && fflush(f) == 0;
    if (f) {
#if defined(_WIN32) || defined(_WIN64)
        ok = _commit(_fileno(f)) == 0 && ok;
#else
        ok = fsync(fileno(f)) == 0 && ok;
#endif
        if (fclose(f) != 0) ok = false;
    }
    if (ok && nk_rename_utf8(tmp, path) != 0) ok = false;
    if (!ok) (void)nk_remove_utf8(tmp);
    free(tmp);
    return ok;
}

/* Where the value of the one "wlan_mac_hex" key starts in the text, in the form the runtime
   writes ("wlan_mac_hex": "<12 hex digits>") and equal to old_hex; 0 when it is not so. */
static size_t console_mac_value_offset(const char *text, size_t len, const char *old_hex) {
    static const char key[] = "\"wlan_mac_hex\"";
    const char *found = NULL;
    size_t found_count = 0u;
    size_t i;
    for (const char *p = text; (p = strstr(p, key)) != NULL; p += sizeof(key) - 1u) {
        found = p;
        found_count++;
    }
    if (found_count != 1u) return 0u;
    i = (size_t)(found - text) + sizeof(key) - 1u;
    while (i < len && text[i] == ' ') i++;
    if (i >= len || text[i] != ':') return 0u;
    i++;
    while (i < len && text[i] == ' ') i++;
    if (i + NK_CONSOLE_ID_HEX_LEN + 2u > len || text[i] != '"' ||
        text[i + 1u + NK_CONSOLE_ID_HEX_LEN] != '"') {
        return 0u;
    }
    for (size_t k = 0; k < NK_CONSOLE_ID_HEX_LEN; k++) {
        if (hex_value(text[i + 1u + k]) != hex_value(old_hex[k])) return 0u;
    }
    return i + 1u;
}

NkConsoleIdStatus nk_adhoc_console_id_renew(const char *registry_path,
                                            char new_hex[NK_CONSOLE_ID_HEX_LEN + 1u]) {
    char old_hex[NK_CONSOLE_ID_HEX_LEN + 1u];
    char check_hex[NK_CONSOLE_ID_HEX_LEN + 1u];
    char *text = NULL;
    size_t len = 0u, value_at;
    bool fresh = false;
    NkConsoleIdStatus status;
    if (!new_hex) return NK_CONSOLE_ID_UNEXPECTED;
    new_hex[0] = '\0';
    status = console_file_read(registry_path, &text, &len);
    if (status != NK_CONSOLE_ID_OK) return status;
    status = console_id_from_text(text, len, old_hex);
    value_at = status == NK_CONSOLE_ID_OK ? console_mac_value_offset(text, len, old_hex) : 0u;
    if (status == NK_CONSOLE_ID_OK && value_at == 0u) status = NK_CONSOLE_ID_UNEXPECTED;
    for (int attempt = 0; status == NK_CONSOLE_ID_OK && attempt < 4 && !fresh; attempt++) {
        uint8_t mac[6];
        if (!console_random_bytes(mac, sizeof(mac))) {
            status = NK_CONSOLE_ID_NO_RANDOM;
            break;
        }
        mac[0] = (uint8_t)(mac[0] & 0xfcu); /* unicast, as the runtime makes it */
        for (size_t b = 0; b < sizeof(mac); b++) {
            new_hex[2u * b] = k_hex_digits[mac[b] >> 4];
            new_hex[2u * b + 1u] = k_hex_digits[mac[b] & 0x0fu];
        }
        new_hex[NK_CONSOLE_ID_HEX_LEN] = '\0';
        fresh = strcmp(new_hex, old_hex) != 0;
    }
    if (status == NK_CONSOLE_ID_OK && !fresh) status = NK_CONSOLE_ID_NO_RANDOM;
    if (status == NK_CONSOLE_ID_OK) {
        memcpy(text + value_at, new_hex, NK_CONSOLE_ID_HEX_LEN);
        /* The changed text must read back as the runtime will read it, with the new value. */
        if (console_id_from_text(text, len, check_hex) != NK_CONSOLE_ID_OK ||
            strcmp(check_hex, new_hex) != 0) {
            status = NK_CONSOLE_ID_UNEXPECTED;
        } else if (!console_file_replace(registry_path, text, len)) {
            status = NK_CONSOLE_ID_WRITE_FAILED;
        }
    }
    free(text);
    if (status != NK_CONSOLE_ID_OK) new_hex[0] = '\0';
    return status;
}

bool nk_adhoc_console_id_display(const char *hex, char *out, size_t out_size) {
    if (!out || out_size < 18u) return false;
    out[0] = '\0';
    if (!hex || strlen(hex) != NK_CONSOLE_ID_HEX_LEN) return false;
    for (size_t i = 0; i < NK_CONSOLE_ID_HEX_LEN; i++)
        if (hex_value(hex[i]) < 0) return false;
    snprintf(out, out_size, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", hex, hex + 2, hex + 4, hex + 6,
             hex + 8, hex + 10);
    return true;
}
