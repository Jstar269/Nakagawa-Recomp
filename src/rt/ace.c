/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace.c - Ace Mode's launch line, stamps, 30 Hz detector, speed readout and per-run report
 * (contract in ace.h). C library only.
 */
#include "ace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>   /* MoveFileExA: the report's atomic replace */
#endif

/* ---- the launch line ------------------------------------------------------------------------ */

static void why_set(char *why, size_t cap, const char *text) {
    if (why && cap) snprintf(why, cap, "%s", text);
}

static int value_on_off(const char *v, int *out) {
    if (strcmp(v, "on") == 0) { *out = 1; return 1; }
    if (strcmp(v, "off") == 0) { *out = 0; return 1; }
    return 0;
}

/* The line's keys. Each says whether a line must carry it, so the order of the table decides
   nothing for the parser: the rows that are not required (tier, mips, stick, load, depth and
   cull) are the keys a line may leave out. Each also names the longest value ace_format writes
   for it, so ace_longest_line builds the longest line from this table alone. */
typedef enum {
    ACE_KEY_AF, ACE_KEY_TEXT, ACE_KEY_READOUT, ACE_KEY_NOTICE, ACE_KEY_WIDE, ACE_KEY_SMOOTH,
    ACE_KEY_SAS, ACE_KEY_TESS, ACE_KEY_TWIN, ACE_KEY_MIPS, ACE_KEY_TIER, ACE_KEY_STICK, ACE_KEY_LOAD,
    ACE_KEY_DEPTH, ACE_KEY_CULL
} AceKey;

static const struct {
    const char *name;
    AceKey key;
    int required;
    const char *longest;
} k_ace_keys[] = {
    {"af", ACE_KEY_AF, 1, "16"},
    {"text", ACE_KEY_TEXT, 1, "off"},
    {"readout", ACE_KEY_READOUT, 1, "off"},
    {"notice", ACE_KEY_NOTICE, 1, "off"},
    {"wide", ACE_KEY_WIDE, 1, "fill"},
    {"smooth", ACE_KEY_SMOOTH, 1, "match"},
    {"sas", ACE_KEY_SAS, 1, "sinc"},
    {"tess", ACE_KEY_TESS, 1, "off"},
    {"twin", ACE_KEY_TWIN, 1, "off"},
    {"mips", ACE_KEY_MIPS, 0, "on"},
    {"tier", ACE_KEY_TIER, 0, "enhanced"},
    {"stick", ACE_KEY_STICK, 0, "126"},
    {"load", ACE_KEY_LOAD, 0, "fast"},
    {"depth", ACE_KEY_DEPTH, 0, "32"},
    {"cull", ACE_KEY_CULL, 0, "on"},
};

size_t ace_longest_line(char *out, size_t cap) {
    size_t used = 0;
    int n;
    if (!out || !cap) return 0;
    n = snprintf(out, cap, "v1");
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    used = (size_t)n;
    for (size_t i = 0; i < sizeof(k_ace_keys) / sizeof(k_ace_keys[0]); i++) {
        if (!k_ace_keys[i].longest) { out[0] = '\0'; return 0; }
        n = snprintf(out + used, cap - used, ";%s=%s", k_ace_keys[i].name, k_ace_keys[i].longest);
        if (n < 0 || (size_t)n >= cap - used) { out[0] = '\0'; return 0; }
        used += (size_t)n;
    }
    return used;
}

/* stick=<counts>: a decimal 1..ACE_STICK_COUNTS_MAX with no sign and no leading zero, so the
   line's text is the one ace_format writes. */
static int value_stick_counts(const char *v, int *out) {
    int n = 0;
    if (!v[0] || v[0] == '0' || strlen(v) > 3) return 0;
    for (const char *c = v; *c; c++) {
        if (*c < '0' || *c > '9') return 0;
        n = n * 10 + (*c - '0');
    }
    if (n < 1 || n > ACE_STICK_COUNTS_MAX) return 0;
    *out = n;
    return 1;
}

int ace_parse(const char *spec, AceConfig *out, char *why, size_t why_cap) {
    enum { KEYS = (int)(sizeof(k_ace_keys) / sizeof(k_ace_keys[0])) };
    AceConfig c;
    char buf[ACE_SPEC_MAX];
    int seen[KEYS] = {0};
    memset(&c, 0, sizeof(c));
    if (out) memset(out, 0, sizeof(*out));
    if (!spec) { why_set(why, why_cap, "unset"); return 0; }
    if (strcmp(spec, "off") == 0) { why_set(why, why_cap, ""); return 1; }
    if (strlen(spec) >= sizeof(buf)) { why_set(why, why_cap, "line too long"); return 0; }
    memcpy(buf, spec, strlen(spec) + 1u);

    char *field = buf;
    int first = 1;
    for (;;) {
        char *end = strchr(field, ';');
        if (end) *end = '\0';
        if (!field[0]) { why_set(why, why_cap, "empty field"); return 0; }
        if (first) {
            if (strcmp(field, "v1") != 0) { why_set(why, why_cap, "unknown version"); return 0; }
            first = 0;
        } else {
            char *eq = strchr(field, '=');
            int k = -1;
            if (!eq || eq == field || !eq[1]) { why_set(why, why_cap, "field is not key=value"); return 0; }
            *eq = '\0';
            const char *v = eq + 1;
            for (int i = 0; i < KEYS; i++)
                if (strcmp(field, k_ace_keys[i].name) == 0) k = i;
            if (k < 0) { why_set(why, why_cap, "unknown key"); return 0; }
            if (seen[k]) { why_set(why, why_cap, "duplicate key"); return 0; }
            seen[k] = 1;
            int ok = 0;
            switch (k_ace_keys[k].key) {
            case ACE_KEY_AF:
                ok = strcmp(v, "0") == 0 || strcmp(v, "2") == 0 || strcmp(v, "4") == 0 ||
                     strcmp(v, "8") == 0 || strcmp(v, "16") == 0;
                if (ok) c.af = atoi(v);
                break;
            case ACE_KEY_TEXT: ok = value_on_off(v, &c.text); break;
            case ACE_KEY_READOUT: ok = value_on_off(v, &c.readout); break;
            case ACE_KEY_NOTICE: ok = value_on_off(v, &c.notice); break;
            case ACE_KEY_WIDE:
                if (strcmp(v, "off") == 0) { c.wide = ACE_WIDE_OFF; ok = 1; }
                else if (strcmp(v, "fill") == 0) { c.wide = ACE_WIDE_FILL; ok = 1; }
                if (ok && c.wide && !ACE_HAS_WIDE) {
                    why_set(why, why_cap, "wide is not part of this build"); return 0;
                }
                break;
            case ACE_KEY_CULL:
                ok = value_on_off(v, &c.cull);
                if (ok && c.cull && !ACE_HAS_CULL) {
                    why_set(why, why_cap, "cull is not part of this build"); return 0;
                }
                break;
            case ACE_KEY_STICK:
                ok = value_stick_counts(v, &c.stick);
                if (ok && !ACE_HAS_STICK) {
                    why_set(why, why_cap, "stick is not part of this build"); return 0;
                }
                break;
            case ACE_KEY_LOAD:
                if (strcmp(v, "off") == 0) { c.load = 0; ok = 1; }
                else if (strcmp(v, "fast") == 0) { c.load = 1; ok = 1; }
                if (ok && c.load && !ACE_HAS_LOAD) {
                    why_set(why, why_cap, "load is not part of this build"); return 0;
                }
                break;
            case ACE_KEY_DEPTH:
                if (strcmp(v, "off") == 0) { c.depth = 0; ok = 1; }
                else if (strcmp(v, "32") == 0) { c.depth = 32; ok = 1; }
                if (ok && c.depth && !ACE_HAS_DEPTH) {
                    why_set(why, why_cap, "depth is not part of this build"); return 0;
                }
                break;
            case ACE_KEY_MIPS:
                ok = value_on_off(v, &c.mips);
                if (ok && c.mips && !ACE_HAS_MIPS) {
                    why_set(why, why_cap, "mips is not part of this build"); return 0;
                }
                break;
            case ACE_KEY_TIER:
                if (strcmp(v, "enhanced") == 0) { c.tier = ACE_TIER_ENHANCED; ok = 1; }
                else if (strcmp(v, "ace") == 0) { c.tier = ACE_TIER_ACE; ok = 1; }
                break;
            case ACE_KEY_SMOOTH:
                if (strcmp(v, "off") == 0) { c.smooth = ACE_SMOOTH_OFF; ok = 1; }
                else if (strcmp(v, "match") == 0) { c.smooth = ACE_SMOOTH_MATCH; ok = 1; }
                else if (strcmp(v, "x2") == 0) { c.smooth = ACE_SMOOTH_X2; ok = 1; }
                if (ok && ((c.smooth && !ACE_HAS_SMOOTH) ||
                           (c.smooth == ACE_SMOOTH_MATCH && !ACE_HAS_SMOOTH_MATCH))) {
                    why_set(why, why_cap, "this smooth mode is not part of this build"); return 0;
                }
                break;
            case ACE_KEY_SAS:
                if (strcmp(v, "off") == 0) { c.sas = ACE_SAS_OFF; ok = 1; }
                else if (strcmp(v, "sinc") == 0) { c.sas = ACE_SAS_SINC; ok = 1; }
                if (ok && c.sas && !ACE_HAS_SAS) {
                    why_set(why, why_cap, "sas is not part of this build"); return 0;
                }
                break;
            case ACE_KEY_TWIN: ok = value_on_off(v, &c.twin); break;
            case ACE_KEY_TESS:
                if (strcmp(v, "off") == 0) { c.tess = 0; ok = 1; }
                else if (strcmp(v, "2") == 0 || strcmp(v, "3") == 0 || strcmp(v, "4") == 0) {
                    c.tess = atoi(v); ok = 1;
                }
                if (ok && c.tess && !ACE_HAS_TESS) {
                    why_set(why, why_cap, "tess is not part of this build"); return 0;
                }
                break;
            }
            if (!ok) { why_set(why, why_cap, "bad value"); return 0; }
        }
        if (!end) break;
        field = end + 1;
    }
    for (int i = 0; i < KEYS; i++)
        if (k_ace_keys[i].required && !seen[i]) { why_set(why, why_cap, "missing key"); return 0; }
    /* An Ace item (contract 11.3.6): only an Ace-tier line may turn it on. */
    if (c.cull && c.tier != ACE_TIER_ACE) { why_set(why, why_cap, "cull needs tier=ace"); return 0; }
    if (c.stick && c.tier != ACE_TIER_ACE) { why_set(why, why_cap, "stick needs tier=ace"); return 0; }
    /* An Ace item (contract 11.3.10): it changes guest timing, for one title's declared waits. */
    if (c.load && c.tier != ACE_TIER_ACE) { why_set(why, why_cap, "load needs tier=ace"); return 0; }
    c.on = 1;
    if (out) *out = c;
    why_set(why, why_cap, "");
    return 1;
}

static const char *on_off(int v) { return v ? "on" : "off"; }
static const char *wide_word(int v) { return v == ACE_WIDE_FILL ? "fill" : "off"; }
static const char *smooth_word(int v) {
    return v == ACE_SMOOTH_MATCH ? "match" : v == ACE_SMOOTH_X2 ? "x2" : "off";
}
static const char *tier_word(int v) { return v == ACE_TIER_ACE ? "ace" : "enhanced"; }
static const char *sas_word(int v) { return v == ACE_SAS_SINC ? "sinc" : "off"; }
static const char *tess_word(int v) { return v == 2 ? "2" : v == 3 ? "3" : v == 4 ? "4" : "off"; }

int ace_format(const AceConfig *c, char *out, size_t cap) {
    int n;
    if (!out || !cap) return 0;
    if (!c || !c->on) n = snprintf(out, cap, "off");
    else n = snprintf(out, cap, "v1;tier=%s;af=%d;text=%s;readout=%s;notice=%s;wide=%s;smooth=%s;"
                                "sas=%s;tess=%s;twin=%s",
                      tier_word(c->tier), c->af, on_off(c->text), on_off(c->readout),
                      on_off(c->notice), wide_word(c->wide), smooth_word(c->smooth),
                      sas_word(c->sas), tess_word(c->tess), on_off(c->twin));
    /* Written only when on, at its place in the key order (right after twin): a missing mips
       is off, so every line without it keeps its text. */
    if (n > 0 && (size_t)n < cap && c && c->on && c->mips) {
        const int m = snprintf(out + n, cap - (size_t)n, ";mips=on");
        n = m < 0 ? -1 : n + m;
    }
    /* Written only when on: a missing stick is off, so every line without it keeps its text. */
    if (n > 0 && (size_t)n < cap && c && c->on && c->stick) {
        const int m = snprintf(out + n, cap - (size_t)n, ";stick=%d", c->stick);
        n = m < 0 ? -1 : n + m;
    }
    /* Likewise load, after stick: a missing load is off. */
    if (n > 0 && (size_t)n < cap && c && c->on && c->load) {
        const int m = snprintf(out + n, cap - (size_t)n, ";load=fast");
        n = m < 0 ? -1 : n + m;
    }
    /* Likewise depth, after load: a missing depth is the PSP's 16 bits. */
    if (n > 0 && (size_t)n < cap && c && c->on && c->depth) {
        const int m = snprintf(out + n, cap - (size_t)n, ";depth=%d", c->depth);
        n = m < 0 ? -1 : n + m;
    }
    /* The same for cull, which stays last: written only when on. */
    if (n > 0 && (size_t)n < cap && c && c->on && c->cull) {
        const int m = snprintf(out + n, cap - (size_t)n, ";cull=on");
        n = m < 0 ? -1 : n + m;
    }
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return 1;
}

/* ---- the run's switch ----------------------------------------------------------------------- */

int g_ace_on;

static AceConfig s_req, s_eff;
static char s_spec[ACE_SPEC_MAX] = "off";
static char s_report_path[1024];
static char s_unavailable[ACE_FEATURE_COUNT][96];
static int s_af_device;          /* the device's level when lower than the request, else 0 */
static int s_confirmed[ACE_FEATURE_COUNT];
static void (*s_exit_report[ACE_FEATURE_COUNT])(void);
static uint64_t s_af_failures;
static unsigned long s_cull_untracked;
static unsigned long long s_stick_samples;   /* samples off rest the stick remap passed through */
static int s_stick_host;                     /* the host dead zone it used last, in centred counts */
static unsigned long long s_load_skipped;
static unsigned s_load_run;          /* consecutive logic updates with a skip, this one included */
static unsigned s_load_run_max;
static int s_load_update_skipped;    /* a wait was skipped since the last update began */
static int s_load_paced;             /* the guard fired: nothing is skipped for the rest of the run */

static const char *const k_feature_key[ACE_FEATURE_COUNT] = {
    "af", "text", "readout", "notice", "wide", "smooth", "sas", "tess", "stick", "load", "cull",
    "depth", "twin", "mips"
};

/* The run's observation state. */
static AceDetector s_det;
static AceReadoutState s_readout;
static AceReadoutView s_view;
static int s_have_view;
static AceCounters s_counters;
static uint64_t s_win_start_ns;
static int s_win_started;
static uint32_t s_win_frames, s_win_skipped, s_win_betweens, s_win_shown, s_win_motion_milli;
static uint64_t s_shown_seq;   /* AcePace.frame_seq of the newest real frame that reached the screen */
static uint64_t s_win_idle_ns;
static uint32_t s_last_flip_vcount, s_last_flip_fb;
static int s_have_flip;
static uint64_t s_rate_vblanks[ACE_RATE_COUNT];
static uint64_t s_limited_windows;
static int s_ff_running;   /* ace_note_fast_forward: set by the scheduler, not by a run reset */
static AcePace s_pace;
static AcePaceStats s_pace_stats;
static uint32_t s_pace_vcount;
static AceSmoothAuxStats s_aux_stats;
static AceMotionStats s_motion;
static int s_have_aux_stats;

#define ACE_TARGETS 8
typedef struct { uint32_t fb, bytes, draws, seen, touched; int used; } AceTarget;
static AceTarget s_targets[ACE_TARGETS];
static uint32_t s_target_clock;

static void observation_reset(void) {
    ace_det_reset(&s_det);
    ace_readout_reset(&s_readout);
    memset(&s_view, 0, sizeof(s_view));
    s_have_view = 0;
    memset(&s_counters, 0, sizeof(s_counters));
    s_win_start_ns = 0; s_win_started = 0;
    s_win_frames = s_win_skipped = s_win_betweens = s_win_shown = 0; s_win_idle_ns = 0;
    s_win_motion_milli = 0;
    memset(&s_motion, 0, sizeof(s_motion));
    s_shown_seq = 0;
    s_last_flip_vcount = s_last_flip_fb = 0; s_have_flip = 0;
    memset(s_rate_vblanks, 0, sizeof(s_rate_vblanks));
    s_limited_windows = 0;
    ace_pace_reset(&s_pace);
    memset(&s_pace_stats, 0, sizeof(s_pace_stats));
    s_pace_vcount = 0;
    memset(&s_aux_stats, 0, sizeof(s_aux_stats));
    s_have_aux_stats = 0;
    memset(s_targets, 0, sizeof(s_targets));
    s_target_clock = 0;
}

int ace_init_spec(const char *spec, const char *report_path) {
    char why[64];
    g_ace_on = 0;
    memset(&s_req, 0, sizeof(s_req));
    memset(&s_eff, 0, sizeof(s_eff));
    memset(s_unavailable, 0, sizeof(s_unavailable));
    memset(s_confirmed, 0, sizeof(s_confirmed));
    memset(s_exit_report, 0, sizeof(s_exit_report));
    s_af_failures = 0;
    s_cull_untracked = 0;
    s_stick_samples = 0;
    s_stick_host = 0;
    s_load_skipped = 0;
    s_load_run = 0;
    s_load_run_max = 0;
    s_load_update_skipped = 0;
    s_load_paced = 0;
    s_af_device = 0;
    s_report_path[0] = '\0';
    snprintf(s_spec, sizeof(s_spec), "off");
    observation_reset();
    if (!spec) return 0;                       /* unset: off, silently */
    if (!ace_parse(spec, &s_req, why, sizeof(why))) {
        fprintf(stderr, "ACE_MODE: refused (%s)\n", why);
        return 0;
    }
    if (!s_req.on) return 0;                   /* "off": authentic, silently */
    s_eff = s_req;
    (void)ace_format(&s_req, s_spec, sizeof(s_spec));
    if (report_path && report_path[0])
        snprintf(s_report_path, sizeof(s_report_path), "%s", report_path);
    g_ace_on = 1;
    fprintf(stderr, "ACE_MODE: on (tier=%s, af=%d, text=%s, readout=%s, notice=%s, wide=%s, "
            "smooth=%s, sas=%s, tess=%s, twin=%s)\n", tier_word(s_req.tier), s_req.af, on_off(s_req.text),
            on_off(s_req.readout), on_off(s_req.notice), wide_word(s_req.wide),
            smooth_word(s_req.smooth), sas_word(s_req.sas), tess_word(s_req.tess),
            on_off(s_req.twin));
    if (s_req.stick)
        fprintf(stderr, "ACE_MODE: stick remap %d (the host stick starts past the game's square dead "
                        "zone of %d counts; the game reads changed stick values)\n",
                s_req.stick, s_req.stick);
    if (s_req.load)
        fprintf(stderr, "ACE_MODE: load fast (a title's declared loading waits return at once; "
                        "guest timing changes)\n");
    if (s_req.depth)
        fprintf(stderr, "ACE_MODE: depth %d (the host depth buffer keeps unrounded depth, so surfaces "
                        "within one 16-bit step of each other can draw in a different order; guest "
                        "depth reads stay 16-bit)\n", s_req.depth);
    if (s_req.cull) fprintf(stderr, "ACE_MODE: cull on (a title's own culling is widened to the wings)\n");
    if (s_req.mips) fprintf(stderr, "ACE_MODE: mips on (textures with one level get host-built mip chains)\n");
    return 1;
}

int ace_init_from_env(void) {
    return ace_init_spec(getenv("SR_ACE"), getenv("SR_ACE_REPORT"));
}

const AceConfig *ace_requested(void) { return &s_req; }
const AceConfig *ace_effective(void) { return &s_eff; }
const char *ace_spec_text(void) { return s_spec; }
const char *ace_tier_name(void) { return s_req.tier == ACE_TIER_ACE ? "Ace" : "Enhanced"; }

void ace_feature_unavailable(AceFeature feature, const char *reason) {
    if (!g_ace_on || (unsigned)feature >= ACE_FEATURE_COUNT) return;
    if (s_unavailable[feature][0]) return;     /* named once */
    snprintf(s_unavailable[feature], sizeof(s_unavailable[feature]), "%s",
             reason && reason[0] ? reason : "not available on this PC");
    switch (feature) {
    case ACE_FEATURE_AF: s_eff.af = 0; break;
    case ACE_FEATURE_TEXT: s_eff.text = 0; break;
    case ACE_FEATURE_READOUT: s_eff.readout = 0; break;
    case ACE_FEATURE_NOTICE: s_eff.notice = 0; break;
    case ACE_FEATURE_WIDE: s_eff.wide = 0; break;
    case ACE_FEATURE_SMOOTH: s_eff.smooth = 0; break;
    case ACE_FEATURE_STICK: s_eff.stick = 0; break;
    case ACE_FEATURE_MIPS: s_eff.mips = 0; break;
    case ACE_FEATURE_SAS: s_eff.sas = 0; break;
    case ACE_FEATURE_TESS: s_eff.tess = 0; break;
    case ACE_FEATURE_LOAD: s_eff.load = 0; break;
    case ACE_FEATURE_CULL: s_eff.cull = 0; break;
    case ACE_FEATURE_DEPTH: s_eff.depth = 0; break;
    case ACE_FEATURE_TWIN: s_eff.twin = 0; break;
    default: break;
    }
    fprintf(stderr, "ACE_MODE: %s unavailable (%s)\n", k_feature_key[feature], s_unavailable[feature]);
}

void ace_set_cull_untracked(unsigned long n) {
    if (g_ace_on) s_cull_untracked = n;
}

/* ---- stick dead-zone remap ------------------------------------------------------------------ */

int ace_stick_host_counts(int deadzone_inner_x, int deadzone_inner_y) {
    const int d = deadzone_inner_x > deadzone_inner_y ? deadzone_inner_x : deadzone_inner_y;
    if (d <= 0) return 0;
    return (int)(((long)d * 255L) / 65535L);
}

/* A host byte as a signed offset from rest (128), at most 127 either way. */
static int stick_offset(uint8_t b) {
    const int h = (int)b - 128;
    return h < -127 ? -127 : h;
}

/* The inverse of the title's centring x = b - (b > 128) - 127. */
static uint8_t stick_game_byte(int x) {
    return (uint8_t)(x >= 1 ? x + 128 : x + 127);
}

/* One axis scaled by num/den, rounded to nearest, its sign kept, as a byte. An axis at rest
   keeps its byte. */
static uint8_t stick_scale_axis(uint8_t b, int h, int num, int den) {
    if (h == 0) return b;
    const int a = h < 0 ? -h : h;
    int s = (2 * a * num + den) / (2 * den);
    if (s > 127) s = 127;
    return stick_game_byte(h < 0 ? -s : s);
}

void ace_stick_remap(uint8_t *lx, uint8_t *ly, int game_counts, int host_counts) {
    const int g = game_counts;
    int d = host_counts;
    if (!lx || !ly || g < 1 || g > ACE_STICK_COUNTS_MAX) return;
    if (d < 0 || d >= 127) d = 0;
    const int hx = stick_offset(*lx), hy = stick_offset(*ly);
    const int ax = hx < 0 ? -hx : hx, ay = hy < 0 ? -hy : hy;
    const int m = ax > ay ? ax : ay;
    if (m == 0) return;
    /* Past the host's dead zone the host's live range maps onto the game's, G..127; up to it
       (a source with a smaller dead zone than the host's, such as a mouse) it stays below G.
       Both hold for any D in 0..126, larger than G or not: 127 - D is at least 1, and the
       second branch runs only with D >= 1. */
    int target = m > d ? g + (m - d) * (127 - g) / (127 - d) : m * (g - 1) / d;
    if (target > 127) target = 127;
    *lx = stick_scale_axis(*lx, hx, target, m);
    *ly = stick_scale_axis(*ly, hy, target, m);
}

void ace_stick_sample(uint8_t *lx, uint8_t *ly, int host_counts) {
    if (!g_ace_on || !s_eff.stick || !lx || !ly) return;
    int d = host_counts;
    if (d < 0 || d >= 127) d = 0;
    s_stick_host = d;
    if (*lx == 128u && *ly == 128u) return;
    ace_stick_remap(lx, ly, s_eff.stick, d);
    s_stick_samples++;
    s_confirmed[ACE_FEATURE_STICK] = 1;
}

int nk_ace_load_fast(const char *site, uint32_t pc) {
    if (!g_ace_on || !s_eff.load) return 0;
    s_confirmed[ACE_FEATURE_LOAD] = 1;   /* a declared wait was reached with the part on */
    if (s_load_paced) return 0;          /* the guard fired: the stock call runs */
    if (!s_load_update_skipped) {
        s_load_update_skipped = 1;
        if (++s_load_run > s_load_run_max) s_load_run_max = s_load_run;
    }
    s_load_skipped++;
    if (s_load_run >= ACE_LOAD_PACING_UPDATES) {
        s_load_paced = 1;
        fprintf(stderr, "ENHANCEMENT_LOAD_PACING: site %s at 0x%08x was skipped in %u logic updates in a row, "
                        "so it paces logic updates, not a load; every load wait runs as stock for the rest "
                        "of the run\n", site ? site : "?", (unsigned)pc, s_load_run);
    }
    return 1;
}

void nk_ace_load_update(void) {
    if (!g_ace_on || !s_eff.load || s_load_paced) return;
    if (!s_load_update_skipped) s_load_run = 0;   /* the update that ended skipped nothing */
    s_load_update_skipped = 0;
}

unsigned long long ace_load_waits_skipped(void) { return s_load_skipped; }
unsigned ace_load_skip_max_run(void) { return s_load_run_max; }
int ace_load_paced(void) { return s_load_paced; }

void ace_confirm(AceFeature feature) {
    if (g_ace_on && (unsigned)feature < ACE_FEATURE_COUNT) s_confirmed[feature] = 1;
}

void ace_note_af_failure(void) {
    if (g_ace_on) s_af_failures++;
}

void ace_set_readout(int on) {
    if (!g_ace_on || s_unavailable[ACE_FEATURE_READOUT][0]) return;
    s_eff.readout = on ? 1 : 0;
}

void ace_set_twin_running(void) {
    if (!g_ace_on || s_unavailable[ACE_FEATURE_TWIN][0]) return;
    s_eff.twin = 1;
    s_confirmed[ACE_FEATURE_TWIN] = 1;
}

void ace_set_af_effective(int af, const char *reason) {
    if (!g_ace_on || !s_req.af) return;
    if (af <= 0) { ace_feature_unavailable(ACE_FEATURE_AF, reason); return; }
    if (af < s_req.af) {
        s_eff.af = af;
        s_af_device = af;
        fprintf(stderr, "ACE_MODE: af %d (the device's most)\n", af);
    }
}

static size_t words_add(char *out, size_t cap, size_t used, const char *word) {
    int n = snprintf(out + used, used < cap ? cap - used : 0, "%s%s",
                     used ? " \xC2\xB7 " : "", word);
    if (n < 0 || used + (size_t)n >= cap) return used;
    return used + (size_t)n;
}

size_t ace_feature_words(char *out, size_t cap) {
    size_t used = 0;
    char af[32];
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!g_ace_on) return 0;
    if (s_eff.af) {
        snprintf(af, sizeof(af), "%dx filtering", s_eff.af);
        used = words_add(out, cap, used, af);
    }
    if (s_eff.text) used = words_add(out, cap, used, "crisp keyboard");
    if (s_eff.wide) used = words_add(out, cap, used, "ultrawide");
    if (s_eff.stick) used = words_add(out, cap, used, "stick remap");
    if (s_eff.load) used = words_add(out, cap, used, "short loads");
    if (s_eff.mips) used = words_add(out, cap, used, "smooth far textures");
    if (s_eff.cull) used = words_add(out, cap, used, "wider culling");
    if (s_eff.depth) used = words_add(out, cap, used, "32-bit depth");
    if (s_eff.smooth) used = words_add(out, cap, used, "smooth motion");
    if (s_eff.sas) used = words_add(out, cap, used, "clean pitched sound");
    if (s_eff.tess) {
        char tess[32];
        snprintf(tess, sizeof(tess), "%dx patches", s_eff.tess);
        used = words_add(out, cap, used, tess);
    }
    if (s_eff.twin) used = words_add(out, cap, used, "full 32-bit colour");
    if (s_eff.readout) used = words_add(out, cap, used, "speed readout");
    return used;
}

/* ---- the 30 Hz detector --------------------------------------------------------------------- */

void ace_det_reset(AceDetector *d) {
    if (d) memset(d, 0, sizeof(*d));
}

AceRate ace_det_window_class(const AceDetector *d) {
    if (!d || d->count < ACE_DET_WINDOW) return ACE_RATE_NONE;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < ACE_DET_WINDOW; i++) sum += d->interval[i];
    for (uint32_t k = 1; k <= 4; k++) {
        uint32_t lo = ACE_DET_WINDOW * k - 2u, hi = ACE_DET_WINDOW * k + 4u, close_by = 0;
        if (sum < lo || sum > hi) continue;
        for (uint32_t i = 0; i < ACE_DET_WINDOW; i++) {
            uint32_t v = d->interval[i];
            if (v + 1u >= k && v <= k + 1u) close_by++;
        }
        if (close_by >= ACE_DET_WINDOW - 4u) return (AceRate)k;
    }
    return ACE_RATE_VARIABLE;
}

const char *ace_rate_name(AceRate rate) {
    switch (rate) {
    case ACE_RATE_K1: return "60 Hz";
    case ACE_RATE_K2: return "30 Hz";
    case ACE_RATE_K3: return "20 Hz";
    case ACE_RATE_K4: return "15 Hz";
    case ACE_RATE_STATIC: return "static";
    case ACE_RATE_VARIABLE: return "variable";
    default: return "none";
    }
}

/* Moves the stable class toward `raw` under the hysteresis rule. Returns 1 when it changed. */
static int det_settle(AceDetector *d, AceRate raw, uint32_t vcount) {
    AceRate before = d->stable;
    if (raw == ACE_RATE_NONE) return 0;        /* window not full yet: nothing to judge */
    /* STATIC keeps the last class: frames that resume at that rate return to it at once. */
    if (d->stable == ACE_RATE_STATIC && raw == d->last_rate) {
        d->stable = raw;
        d->candidate = ACE_RATE_NONE;
        d->disagreeing = 0;
        return 1;
    }
    if (raw == d->stable) {
        d->disagreeing = 0;
        d->candidate = ACE_RATE_NONE;
        return 0;
    }
    /* STATIC is already ACE_DET_STATIC_VBLANKS of evidence: entered at once. */
    if (raw == ACE_RATE_STATIC) {
        d->stable = raw;
        d->candidate = ACE_RATE_NONE;
        d->disagreeing = 0;
        return 1;
    }
    if (raw != d->candidate) {
        d->candidate = raw;
        d->candidate_since = vcount;
    }
    if (d->stable != ACE_RATE_NONE) {
        if (!d->disagreeing) {
            d->disagreeing = 1;
            d->disagree_since = vcount;
        }
        if (vcount - d->disagree_since >= ACE_DET_LEAVE_VBLANKS) {
            d->stable = ACE_RATE_NONE;
            d->disagreeing = 0;
        }
    }
    /* Any other class needs 1.0 s of agreement. */
    if (d->stable == ACE_RATE_NONE && vcount - d->candidate_since >= ACE_DET_ENTER_VBLANKS) {
        d->stable = raw;
        d->candidate = ACE_RATE_NONE;
        d->disagreeing = 0;
    }
    if (d->stable >= ACE_RATE_K1 && d->stable <= ACE_RATE_K4) {
        d->last_rate = d->stable;
        d->ever_stable = 1;
    }
    return d->stable != before;
}

int ace_det_flip(AceDetector *d, uint32_t vcount, int new_frame) {
    if (!d) return 0;
    if (!new_frame) {
        if (d->have_last && vcount - d->last_new_vcount >= ACE_DET_STATIC_VBLANKS)
            return det_settle(d, ACE_RATE_STATIC, vcount);
        return 0;
    }
    if (d->have_last) {
        uint32_t gap = vcount - d->last_new_vcount;
        /* A gap of a static stretch or longer is not a frame interval: the window keeps the
         * rate the game had, and frames that resume are judged from here. */
        if (gap < ACE_DET_STATIC_VBLANKS) {
            d->interval[d->head] = (uint8_t)gap;
            d->head = (d->head + 1u) % ACE_DET_WINDOW;
            if (d->count < ACE_DET_WINDOW) d->count++;
        }
    }
    d->have_last = 1;
    d->last_new_vcount = vcount;
    return det_settle(d, ace_det_window_class(d), vcount);
}

/* ---- the speed readout ---------------------------------------------------------------------- */

#define ACE_VBLANK_HZ (60000.0 / 1001.0)

void ace_readout_reset(AceReadoutState *s) {
    if (s) memset(s, 0, sizeof(*s));
}

static int rate_k(AceRate r) {
    return r >= ACE_RATE_K1 && r <= ACE_RATE_K4 ? (int)r : 0;
}

void ace_readout_judge(AceReadoutState *s, const AceWindow *w, AceReadoutView *out) {
    AceReadoutView v;
    memset(&v, 0, sizeof(v));
    if (!s || !w || !out) return;
    double secs = w->wall_ns ? (double)w->wall_ns / 1e9 : 1.0;
    v.fps = (double)w->new_frames / secs;
    v.idle_share = w->wall_ns ? (double)w->idle_ns / (double)w->wall_ns : 0.0;
    if (v.idle_share > 1.0) v.idle_share = 1.0;
    /* The class frames are judged against: the stretch's while inside one, else the one now
     * stable, else the last stable one. */
    int k_now = rate_k(w->stable) ? rate_k(w->stable) : rate_k(w->last_rate);
    int k_ref = s->in_stretch && s->stretch_k ? s->stretch_k : k_now;
    int slow = k_ref && v.fps < (ACE_VBLANK_HZ / (double)k_ref) * 0.97;
    v.pc_limited = w->skipped_slots > 0 || (v.idle_share < 0.02 && slow);
    if (v.pc_limited && !s->in_stretch) {
        s->in_stretch = 1;
        s->stretch_k = w->ever_stable ? k_now : 0;
    } else if (!v.pc_limited) {
        s->in_stretch = 0;
        s->stretch_k = 0;
    }
    int fps_shown = (int)(v.fps + 0.5);
    if (!w->ever_stable || (v.pc_limited && !s->stretch_k)) {
        v.have_target = 0;
        v.speed_pct = 100;
        if (v.pc_limited) snprintf(v.text, sizeof(v.text), "\xE2\x96\xB2 \xE2\x80\x94  %d FPS  PC LIMITED", fps_shown);
        else snprintf(v.text, sizeof(v.text), "\xE2\x80\x94 %d FPS", fps_shown);
    } else if (v.pc_limited) {
        v.have_target = 1;
        v.target_fps = ACE_VBLANK_HZ / (double)s->stretch_k;
        double pct = 100.0 * v.fps / v.target_fps;
        v.speed_pct = pct >= 100.0 ? 100 : (int)pct;
        snprintf(v.text, sizeof(v.text), "\xE2\x96\xB2 %d%%  %d FPS  PC LIMITED", v.speed_pct, fps_shown);
    } else {
        v.have_target = 1;
        v.target_fps = k_now ? ACE_VBLANK_HZ / (double)k_now : 0.0;
        v.speed_pct = 100;
        snprintf(v.text, sizeof(v.text), "100%%  %d FPS", fps_shown);
    }
    if (w->smooth) {
        v.smooth_on = 1;
        v.shown_fps = (double)w->shown / secs;
        /* What moved: the real pictures, and each in-between by its measured weight. */
        uint32_t real = w->shown >= w->betweens ? w->shown - w->betweens : 0u;
        /* A measurement is read a few presents after its in-between, so it can land in the next
         * window; no window is credited with more motion than the in-betweens it showed. */
        uint64_t motion_milli = w->motion_milli;
        if (motion_milli > 1000ull * w->betweens) motion_milli = 1000ull * w->betweens;
        v.motion_fps = ((double)real + (double)motion_milli / 1000.0) / secs;
        size_t len = strlen(v.text);
        if (w->betweens)
            snprintf(v.text + len, sizeof(v.text) - len, "  SMOOTH %d", (int)(v.motion_fps + 0.5));
        else
            snprintf(v.text + len, sizeof(v.text) - len, "  SMOOTH OFF");
    }
    *out = v;
}

/* ---- observation feed ----------------------------------------------------------------------- */

static AceTarget *target_slot(uint32_t fb, int create) {
    AceTarget *free_slot = NULL, *oldest = &s_targets[0];
    for (int i = 0; i < ACE_TARGETS; i++) {
        AceTarget *t = &s_targets[i];
        if (t->used && t->fb == fb) return t;
        if (!t->used && !free_slot) free_slot = t;
        if (t->touched < oldest->touched) oldest = t;
    }
    if (!create) return NULL;
    AceTarget *t = free_slot ? free_slot : oldest;
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->fb = fb;
    return t;
}

/* The slot for `fb`, marked as the most recently used: eviction takes the oldest. */
static AceTarget *target_touch(uint32_t fb) {
    AceTarget *t = target_slot(fb, 1);
    t->touched = ++s_target_clock;
    return t;
}

void ace_note_target_draw(uint32_t fb) {
    if (!g_ace_on) return;
    AceTarget *t = target_touch(fb);
    t->draws++;
    s_counters.draws++;
}

void ace_note_vram_write(uint32_t off, uint32_t bytes) {
    if (!g_ace_on || !bytes) return;
    uint64_t lo = off, hi = (uint64_t)off + bytes;
    for (int i = 0; i < ACE_TARGETS; i++) {
        AceTarget *t = &s_targets[i];
        if (!t->used || !t->bytes) continue;
        if (lo < (uint64_t)t->fb + t->bytes && t->fb < hi) {
            t->draws++;
            s_counters.draws++;
        }
    }
}

void ace_note_host_wait(uint64_t ns) {
    if (!g_ace_on) return;
    s_win_idle_ns += ns;
    s_counters.host_wait_ns += ns;
}

void ace_note_fast_forward(int on) {
    if (!g_ace_on) return;
    s_ff_running = on != 0;
    s_have_view = 0;
    s_win_started = 0;   /* the first flip at normal speed starts a new window */
}

void ace_note_skipped_slots(uint32_t slots) {
    if (!g_ace_on || !slots || s_ff_running) return;
    s_win_skipped += slots;
    s_counters.skipped_slots += slots;
}

int ace_flip(uint32_t fb, uint32_t bytes, uint32_t vcount, uint64_t now_ns) {
    if (!g_ace_on) return 0;
    AceTarget *t = target_touch(fb);
    t->bytes = bytes;
    /* "seen" holds the draw count at the target's previous flip; the high bit marks a target
     * flipped at least once, so its first flip is new. */
    int new_frame = (t->draws & 0x7FFFFFFFu) != (t->seen & 0x7FFFFFFFu) ||
                    !(t->seen & 0x80000000u) || (s_have_flip && fb != s_last_flip_fb);
    t->seen = (t->draws & 0x7FFFFFFFu) | 0x80000000u;
    if (s_have_flip) s_rate_vblanks[s_det.stable] += (uint64_t)(vcount - s_last_flip_vcount);
    s_last_flip_vcount = vcount;
    s_last_flip_fb = fb;
    s_have_flip = 1;
    s_counters.flips++;
    if (new_frame) { s_counters.new_frames++; s_win_frames++; }
    if (ace_det_flip(&s_det, vcount, new_frame))
        fprintf(stderr, "ACE_RATE: %s\n", ace_rate_name(s_det.stable));
    s_pace_vcount = vcount;
    /* Fast forward makes no in-betweens: each new frame is shown and kept, as at every rate but
     * 30, so the first in-between after it pairs two frames shown one after the other. A frame
     * still held (or owed to a busy display) when fast forward starts is superseded by the first
     * new frame: an OWED verdict would hold that new frame for a VBLANK, and with the display
     * busy each frame after it would be owed in turn for the whole of fast forward. */
    if (s_ff_running && new_frame && s_pace.held) {
        s_pace.superseded++;
        s_pace.held = 0;
        s_pace.owed = 0;
    }
    ace_pace_flip(&s_pace, new_frame, s_ff_running ? ACE_RATE_NONE : s_det.stable, s_eff.smooth, now_ns);
    if (s_ff_running) {
        /* Fast forward: no window is judged (ace_note_fast_forward). */
    } else if (!s_win_started) {
        s_win_started = 1;
        s_win_start_ns = now_ns;
        s_win_frames = 0; s_win_skipped = 0; s_win_betweens = 0; s_win_shown = 0; s_win_idle_ns = 0;
        s_win_motion_milli = 0;
    } else if (now_ns - s_win_start_ns >= 1000000000ull) {
        AceWindow w;
        w.wall_ns = now_ns - s_win_start_ns;
        w.new_frames = s_win_frames;
        w.idle_ns = s_win_idle_ns;
        w.skipped_slots = s_win_skipped;
        w.stable = s_det.stable;
        w.ever_stable = s_det.ever_stable;
        w.last_rate = s_det.last_rate;
        w.smooth = s_req.smooth != 0;  /* asked for: off for the run (no GPU) still says so */
        w.betweens = s_win_betweens;
        w.shown = s_win_shown;
        w.motion_milli = s_win_motion_milli;
        ace_readout_judge(&s_readout, &w, &s_view);
        s_have_view = 1;
        s_counters.windows++;
        if (s_view.pc_limited) s_limited_windows++;
        s_win_start_ns = now_ns;
        s_win_frames = 0; s_win_skipped = 0; s_win_betweens = 0; s_win_shown = 0; s_win_idle_ns = 0;
        s_win_motion_milli = 0;
    }
    return new_frame;
}

int ace_readout_view(AceReadoutView *out) {
    if (!g_ace_on || !s_have_view) return 0;
    if (out) *out = s_view;
    return 1;
}

AceRate ace_rate_stable(void) { return g_ace_on ? s_det.stable : ACE_RATE_NONE; }

void ace_counters(AceCounters *out) {
    if (out) *out = s_counters;
}

/* ---- smooth motion: which frame each present shows ------------------------------------------- */

void ace_pace_reset(AcePace *p) {
    if (p) memset(p, 0, sizeof(*p));
}

void ace_pace_flip(AcePace *p, int new_frame, AceRate rate, int smooth, uint64_t now_ns) {
    p->flip_ns = now_ns;
    if (smooth && new_frame) p->frame_seq++;
    /* An owed frame is shown before anything newer, never replaced (ace.h). */
    const int owed = smooth && p->held && p->owed;
    if (!new_frame) {
        /* The frame on the screen's buffer is shown again. It is the held frame only when the
         * pass kept the newest frame flipped. After flips whose presents were dropped it is a
         * newer real frame, and this present is that frame's first: it shows an owed frame and
         * holds the newer one, owed in turn, or replaces a held frame not yet due. Otherwise the
         * held frame stays held until a present shows it. */
        if (p->held && p->taken_seq != p->frame_seq) {
            p->reflip = 0;
            if (owed) {
                p->show = ACE_SHOW_OWED;
                p->owed_next = 1;
                return;
            }
            p->superseded++;
            p->held = 0;
            p->owed = 0;
        }
        p->reflip = p->held;
        p->show = ACE_SHOW_REAL;
        return;
    }
    p->reflip = 0;
    if (owed) {
        p->show = ACE_SHOW_OWED;
        p->owed_next = 0;
        return;
    }
    if (p->held) {
        p->superseded++;
        p->held = 0;
        p->owed = 0;
    }
    if (!smooth) p->show = ACE_SHOW_REAL;
    else p->show = rate == ACE_RATE_K2 ? ACE_SHOW_BETWEEN : ACE_SHOW_KEEP;
}

AceShow ace_pace_take(AcePace *p) {
    AceShow show = p->show;
    p->show = ACE_SHOW_REAL;
    if (show == ACE_SHOW_REAL) return show;
    int consecutive = p->taken_seq + 1u == p->frame_seq;
    if (show == ACE_SHOW_BETWEEN && !consecutive) {
        show = ACE_SHOW_KEEP;
        p->unpaired++;
    } else if (show == ACE_SHOW_OWED && !p->held) {
        /* A verdict no present took before a VBLANK showed the held frame: nothing is owed. */
        show = ACE_SHOW_KEEP;
    }
    if (show == ACE_SHOW_OWED) p->owed_seq = p->taken_seq;
    p->taken_seq = p->frame_seq;
    return show;
}

void ace_pace_owed_failed(AcePace *p) {
    if (!p->held) return;
    p->show = ACE_SHOW_OWED;
    p->taken_seq = p->owed_seq;
    p->owed = 1;
    p->busy++;
}

void ace_pace_shown_between(AcePace *p, uint32_t vcount, uint64_t now_ns) {
    p->held = 1;
    p->owed = 0;
    p->held_after = vcount;
    p->held_ns = now_ns;
}

void ace_pace_shown_owed(AcePace *p, uint32_t vcount, uint64_t now_ns) {
    p->owed_ns = p->held_ns;
    p->owed_shown = 1;
    p->held = 1;
    /* Shown by a re-flip of the frame now held, whose own present that was: it is owed. */
    p->owed = p->owed_next;
    p->owed_next = 0;
    p->held_after = vcount;
    p->held_ns = now_ns;
}

AcePresentKind ace_pace_flip_kind(const AcePace *p) {
    return p->reflip ? ACE_PRESENT_HELD : ACE_PRESENT_REAL;
}

int ace_pace_vblank(AcePace *p, uint32_t vcount) {
    return p->held && vcount != p->held_after;
}

uint64_t ace_pace_held_shown(AcePace *p) {
    if (p->owed_shown) {
        /* The owed frame, shown by the flip of the frame after it: that one stays held. */
        p->owed_shown = 0;
        return p->owed_ns;
    }
    p->held = 0;
    p->owed = 0;
    return p->held_ns;
}

void ace_pace_held_missed(AcePace *p, int retry) {
    if (!p->held) return;
    if (retry) {
        p->owed = 1;
        p->busy++;
    } else {
        p->superseded++;
        p->held = 0;
        p->owed = 0;
    }
}

AceShow ace_smooth_take(void) {
    return g_ace_on ? ace_pace_take(&s_pace) : ACE_SHOW_REAL;
}

void ace_smooth_shown_between(uint64_t now_ns) {
    if (!g_ace_on) return;
    ace_pace_shown_between(&s_pace, s_pace_vcount, now_ns);
}

void ace_smooth_shown_owed(uint64_t now_ns) {
    if (!g_ace_on) return;
    ace_pace_shown_owed(&s_pace, s_pace_vcount, now_ns);
}

void ace_smooth_owed_failed(void) {
    if (g_ace_on) ace_pace_owed_failed(&s_pace);
}

int ace_smooth_vblank(uint32_t vcount) {
    return g_ace_on && ace_pace_vblank(&s_pace, vcount);
}

void ace_smooth_held_missed(int retry) {
    if (g_ace_on) ace_pace_held_missed(&s_pace, retry);
}

void ace_smooth_flip_busy(void) {
    if (!g_ace_on || !s_pace.reflip) return;
    s_pace.reflip = 0;
    ace_pace_held_missed(&s_pace, 1);
}

void ace_pace_stats_add(AcePaceStats *s, AcePresentKind kind, uint64_t now_ns) {
    if ((unsigned)kind >= ACE_PRESENT_KINDS) return;
    s->kind[kind]++;
    if (s->last_ns && now_ns > s->last_ns) {
        uint64_t ns = now_ns - s->last_ns;
        uint64_t bin = ns / 1000000u;
        s->interval[bin < ACE_PACE_BINS ? bin : ACE_PACE_BINS - 1u]++;
        s->interval_count++;
        s->interval_sum_ns += ns;
    }
    s->last_ns = now_ns;
}

void ace_smooth_note_present(AcePresentKind kind, uint64_t now_ns) {
    if (!g_ace_on) return;
    if (kind == ACE_PRESENT_REAL) {
        kind = ace_pace_flip_kind(&s_pace);
        s_pace.reflip = 0;
    }
    ace_pace_stats_add(&s_pace_stats, kind, now_ns);
    /* The window's pictures that reached the screen: every in-between, the held frame the first
     * time it shows, and a flip's own frame unless it is one already shown. */
    if (kind == ACE_PRESENT_BETWEEN) {
        s_win_betweens++;
        s_win_shown++;
    } else if (kind == ACE_PRESENT_HELD || s_pace.frame_seq != s_shown_seq) {
        s_win_shown++;
        s_shown_seq = s_pace.frame_seq;
    }
    uint64_t held_since = kind == ACE_PRESENT_HELD ? ace_pace_held_shown(&s_pace) : 0u;
    if (kind == ACE_PRESENT_HELD && now_ns > held_since) {
        uint64_t ns = now_ns - held_since;
        s_pace_stats.hold_count++;
        s_pace_stats.hold_sum_ns += ns;
        if (ns > s_pace_stats.hold_max_ns) s_pace_stats.hold_max_ns = ns;
    }
}

void ace_pace_stats(AcePaceStats *out) {
    if (!out) return;
    *out = s_pace_stats;
    out->superseded = s_pace.superseded;
    out->busy = s_pace.busy;
}

/* ---- smooth motion: what an in-between moved ------------------------------------------------- */

uint32_t ace_motion_weight_milli(uint32_t sum_old, uint32_t sum_new) {
    uint64_t total = (uint64_t)sum_old + (uint64_t)sum_new;
    uint64_t least = sum_old < sum_new ? sum_old : sum_new;
    if (!total) return 0u;
    return (uint32_t)((2000u * least + total / 2u) / total);
}

void ace_smooth_note_motion(uint32_t moving, uint32_t eq_old, uint32_t eq_new, uint32_t sum_old,
                            uint32_t sum_new, uint32_t novel) {
    if (!g_ace_on) return;
    uint32_t weight = ace_motion_weight_milli(sum_old, sum_new);
    s_motion.measured++;
    s_motion.moving += moving;
    s_motion.eq_old += eq_old;
    s_motion.eq_new += eq_new;
    s_motion.sum_old += sum_old;
    s_motion.sum_new += sum_new;
    s_motion.novel += novel;
    s_motion.weight_milli += weight;
    s_win_motion_milli += weight;
}

void ace_smooth_note_motion_unread(void) {
    if (g_ace_on) s_motion.unread++;
}

void ace_motion_stats(AceMotionStats *out) {
    if (out) *out = s_motion;
}

void ace_motion_queue_reset(AceMotionQueue *q) {
    if (q) memset(q, 0, sizeof(*q));
}

int ace_motion_queue_made(AceMotionQueue *q, uint64_t serial, uint64_t submission) {
    if (q->count == ACE_MOTION_PENDING) {
        q->dropped++;
        return 0;
    }
    uint32_t at = (q->head + q->count) % ACE_MOTION_PENDING;
    q->serial[at] = serial;
    q->submission[at] = submission;
    q->count++;
    return 1;
}

uint64_t ace_motion_queue_due(AceMotionQueue *q, uint64_t completed) {
    /* Submissions finish in order, so the oldest entry is the first that can be due. */
    if (!q->count || completed < q->submission[q->head]) return 0u;
    uint64_t serial = q->serial[q->head];
    q->head = (q->head + 1u) % ACE_MOTION_PENDING;
    q->count--;
    return serial;
}

/* ---- the per-run report -------------------------------------------------------------------- */

static AceRate dominant_rate(void) {
    AceRate best = ACE_RATE_NONE;
    uint64_t most = 0;
    for (int r = ACE_RATE_K1; r < ACE_RATE_COUNT; r++) {
        if (s_rate_vblanks[r] > most) { most = s_rate_vblanks[r]; best = (AceRate)r; }
    }
    return best;
}

void ace_smooth_aux_report(const AceSmoothAuxStats *s) {
    if (!s) return;
    s_aux_stats = *s;
    s_have_aux_stats = 1;
}

static uint64_t aux_dropped(const AceSmoothAuxStats *a) {
    return a->boxes_large + a->boxes_target + a->boxes_outside + a->boxes_full;
}

size_t ace_report_json(char *out, size_t cap) {
    size_t used = 0;
    int n;
#define ACE_PUT(...) do { \
        n = snprintf(out + used, used < cap ? cap - used : 0, __VA_ARGS__); \
        if (n < 0 || used + (size_t)n >= cap) { if (cap) out[0] = '\0'; return 0; } \
        used += (size_t)n; \
    } while (0)
    if (!out || !cap) return 0;
    ACE_PUT("{\n  \"schema\": 1,\n  \"spec\": \"%s\",\n", s_spec);
    ACE_PUT("  \"effective\": {\"af\": %d, \"text\": %s, \"readout\": %s, \"notice\": %s, "
            "\"wide\": \"%s\", \"smooth\": \"%s\", \"sas\": \"%s\", \"tess\": \"%s\", "
            "\"twin\": %s},\n",
            s_eff.af, s_eff.text ? "true" : "false", s_eff.readout ? "true" : "false",
            s_eff.notice ? "true" : "false", wide_word(s_eff.wide), smooth_word(s_eff.smooth),
            sas_word(s_eff.sas), tess_word(s_eff.tess), s_eff.twin ? "true" : "false");
    if (s_req.stick) ACE_PUT("  \"stick\": {\"counts\": %d, \"host_dead_zone\": %d, "
                             "\"samples_remapped\": %llu},\n",
                             s_req.stick, s_stick_host, s_stick_samples);
    if (s_req.load) ACE_PUT("  \"load\": %s,\n  \"load_waits_skipped\": %llu,\n  \"load_skip_max_run\": %u,\n"
                            "  \"load_paced\": %s,\n",
                            s_eff.load ? "true" : "false", s_load_skipped, s_load_run_max,
                            s_load_paced ? "true" : "false");
    if (s_req.depth) ACE_PUT("  \"depth\": %d,\n", s_eff.depth ? s_eff.depth : 16);
    if (s_req.mips) ACE_PUT("  \"mips\": %s,\n", s_eff.mips ? "true" : "false");
    if (s_req.cull) ACE_PUT("  \"cull\": %s,\n  \"cull_untracked\": %lu,\n",
                            s_eff.cull ? "true" : "false", s_cull_untracked);
    ACE_PUT("  \"unavailable\": {");
    int first = 1;
    for (int f = 0; f < ACE_FEATURE_COUNT; f++) {
        if (!s_unavailable[f][0]) continue;
        /* Reasons are runtime-authored words; quotes and backslashes are dropped. */
        char clean[96];
        size_t j = 0;
        for (size_t i = 0; s_unavailable[f][i] && j + 1 < sizeof(clean); i++) {
            char ch = s_unavailable[f][i];
            if (ch != '"' && ch != '\\' && (unsigned char)ch >= 0x20) clean[j++] = ch;
        }
        clean[j] = '\0';
        ACE_PUT("%s\"%s\": \"%s\"", first ? "" : ", ", k_feature_key[f], clean);
        first = 0;
    }
    ACE_PUT("},\n");
    if (s_af_device) ACE_PUT("  \"af_device_max\": %d,\n", s_af_device);
    if (s_af_failures) ACE_PUT("  \"af_sampler_failures\": %llu,\n", (unsigned long long)s_af_failures);
    ACE_PUT("  \"rate\": {\"dominant\": \"%s\", \"seconds\": {", ace_rate_name(dominant_rate()));
    first = 1;
    for (int r = ACE_RATE_K1; r < ACE_RATE_COUNT; r++) {
        if (!s_rate_vblanks[r]) continue;
        ACE_PUT("%s\"%s\": %.1f", first ? "" : ", ", ace_rate_name((AceRate)r),
                (double)s_rate_vblanks[r] / ACE_VBLANK_HZ);
        first = 0;
    }
    ACE_PUT("}},\n");
    ACE_PUT("  \"frames\": {\"flips\": %llu, \"new\": %llu},\n",
            (unsigned long long)s_counters.flips, (unsigned long long)s_counters.new_frames);
    if (s_req.smooth) {
        const AcePaceStats *ps = &s_pace_stats;
        ACE_PUT("  \"presents\": {\"real\": %llu, \"between\": %llu, \"held\": %llu, "
                "\"superseded\": %llu, \"unpaired\": %llu, \"busy\": %llu},\n",
                (unsigned long long)ps->kind[ACE_PRESENT_REAL],
                (unsigned long long)ps->kind[ACE_PRESENT_BETWEEN],
                (unsigned long long)ps->kind[ACE_PRESENT_HELD],
                (unsigned long long)s_pace.superseded, (unsigned long long)s_pace.unpaired,
                (unsigned long long)s_pace.busy);
        ACE_PUT("  \"added_delay_ms\": {\"mean\": %.2f, \"max\": %.2f},\n",
                ps->hold_count ? (double)ps->hold_sum_ns / (double)ps->hold_count / 1e6 : 0.0,
                (double)ps->hold_max_ns / 1e6);
    }
    if (s_req.smooth && (s_motion.measured || s_motion.unread)) {
        const AceMotionStats *m = &s_motion;
        uint64_t sums = m->sum_old + m->sum_new;
        ACE_PUT("  \"smooth_motion\": {\"measured\": %llu, \"unread\": %llu, \"moving_px\": %llu, "
                "\"old_px\": %llu, \"new_px\": %llu, \"novel_px\": %llu, \"phase\": %.3f, "
                "\"weight\": %.3f},\n",
                (unsigned long long)m->measured, (unsigned long long)m->unread,
                (unsigned long long)m->moving, (unsigned long long)m->eq_old,
                (unsigned long long)m->eq_new, (unsigned long long)m->novel,
                sums ? (double)m->sum_old / (double)sums : 0.0,
                m->measured ? (double)m->weight_milli / 1000.0 / (double)m->measured : 0.0);
    }
    if (s_req.smooth && s_have_aux_stats) {
        const AceSmoothAuxStats *a = &s_aux_stats;
        ACE_PUT("  \"smooth_aux\": {\"pairs\": %llu, \"pairs_mask\": %llu, \"pairs_depth\": %llu, "
                "\"depth_refused\": %llu,\n",
                (unsigned long long)a->pairs, (unsigned long long)a->pairs_mask,
                (unsigned long long)a->pairs_depth, (unsigned long long)a->depth_refused);
        ACE_PUT("    \"boxes\": {\"kept\": %llu, \"merged\": %llu, \"large\": %llu, \"target\": %llu, "
                "\"outside\": %llu, \"full\": %llu},\n",
                (unsigned long long)a->boxes_kept, (unsigned long long)a->boxes_merged,
                (unsigned long long)a->boxes_large, (unsigned long long)a->boxes_target,
                (unsigned long long)a->boxes_outside, (unsigned long long)a->boxes_full);
        ACE_PUT("    \"frames\": {\"mask\": %llu, \"overflow\": %llu, \"depth\": %llu, "
                "\"depth_unwritten\": %llu, \"depth_none\": %llu, \"unshown\": %llu}},\n",
                (unsigned long long)a->frames_mask, (unsigned long long)a->frames_overflow,
                (unsigned long long)a->frames_depth, (unsigned long long)a->frames_depth_unwritten,
                (unsigned long long)a->frames_depth_none, (unsigned long long)a->published_unshown);
    }
    if (s_pace_stats.interval_count) {
        const AcePaceStats *ps = &s_pace_stats;
        ACE_PUT("  \"present_interval_ms\": {\"count\": %llu, \"mean\": %.2f, \"bins\": [",
                (unsigned long long)ps->interval_count,
                (double)ps->interval_sum_ns / (double)ps->interval_count / 1e6);
        for (uint32_t i = 0; i < ACE_PACE_BINS; i++)
            ACE_PUT("%s%llu", i ? ", " : "", (unsigned long long)ps->interval[i]);
        ACE_PUT("]},\n");
    }
    ACE_PUT("  \"pc_limited_seconds\": %llu\n}\n", (unsigned long long)s_limited_windows);
#undef ACE_PUT
    return used;
}

void ace_set_exit_report(AceFeature feature, void (*report)(void)) {
    if ((int)feature >= 0 && feature < ACE_FEATURE_COUNT) s_exit_report[feature] = report;
}

void ace_finish(void) {
    char json[ACE_REPORT_MAX];
    if (!g_ace_on) return;
    for (int f = 0; f < ACE_FEATURE_COUNT; f++)
        if (s_exit_report[f]) s_exit_report[f]();
    /* A feature that is on but never ran: its renderer or presenter did not start. */
    static const int has[ACE_FEATURE_COUNT] = { 1, 1, 1, 1, ACE_HAS_WIDE, ACE_HAS_SMOOTH, ACE_HAS_SAS, ACE_HAS_TESS,
                                                ACE_HAS_STICK, ACE_HAS_LOAD, ACE_HAS_CULL,
                                                ACE_HAS_DEPTH, 1, ACE_HAS_MIPS };
    const int on[ACE_FEATURE_COUNT] = { s_eff.af > 0, s_eff.text, s_eff.readout, s_eff.notice,
                                        s_eff.wide, s_eff.smooth, s_eff.sas, s_eff.tess, s_eff.stick,
                                        s_eff.load, s_eff.cull, s_eff.depth, s_eff.twin,
                                        s_eff.mips };
    for (int f = 0; f < ACE_FEATURE_COUNT; f++) {
        if (has[f] && on[f] && !s_confirmed[f])
            ace_feature_unavailable((AceFeature)f, f == ACE_FEATURE_CULL
                ? "did not run: this title has no culling package in its build, or Widescreen is off"
                : f == ACE_FEATURE_STICK
                ? "did not run: no live stick sample left its rest position in analog mode"
                : f == ACE_FEATURE_LOAD
                ? "did not run: this title's build declares no load part, or none of its waits was reached"
                : "did not run: its renderer or presenter did not start");
    }
    size_t len = ace_report_json(json, sizeof(json));
    fprintf(stderr, "ACE_MODE: exit (%s; rate %s)\n", s_spec, ace_rate_name(dominant_rate()));
    if (s_req.load) fprintf(stderr, "ACE_MODE: load waits skipped %llu (longest run %u logic updates; %s)\n",
                            s_load_skipped, s_load_run_max,
                            s_load_paced ? "paced updates, skipping stopped" : "no pacing seen");
    if (s_req.smooth && s_have_aux_stats) {
        const AceSmoothAuxStats *a = &s_aux_stats;
        fprintf(stderr, "ACE_MODE: smooth inputs (pairs %llu: HUD mask %llu, depth %llu; HUD boxes kept "
                        "%llu, dropped %llu: %llu large, %llu sampling a target, %llu off screen, %llu past "
                        "the limit; frames without a mask %llu, without depth %llu)\n",
                (unsigned long long)a->pairs, (unsigned long long)a->pairs_mask,
                (unsigned long long)a->pairs_depth, (unsigned long long)a->boxes_kept,
                (unsigned long long)aux_dropped(a), (unsigned long long)a->boxes_large,
                (unsigned long long)a->boxes_target, (unsigned long long)a->boxes_outside,
                (unsigned long long)a->boxes_full, (unsigned long long)a->frames_overflow,
                (unsigned long long)(a->frames_depth_unwritten + a->frames_depth_none + a->depth_refused));
    }
    if (!len || !s_report_path[0]) return;
    char tmp[sizeof(s_report_path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_report_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "ACE_MODE: report not written (%s)\n", "cannot create the file");
        return;
    }
    int ok = fwrite(json, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    /* The new report replaces the old one in one step, so a crash or a failure leaves the old
     * report whole. rename() replaces atomically on POSIX; on Windows it refuses an existing
     * target, and MoveFileEx replaces it. */
    if (ok) {
#if defined(_WIN32)
        ok = MoveFileExA(tmp, s_report_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(tmp, s_report_path) == 0;
#endif
    }
    if (!ok) {
        (void)remove(tmp);
        fprintf(stderr, "ACE_MODE: report not written (%s)\n", "write failed");
    }
}

/* ---- fast forward's keys (ace.h) ----------------------------------------------------------- */
/* Set by the presenter's event pump and read by the scheduler's VBLANK, both on the thread that
 * runs the guest, so plain flags are enough. */
static int s_ff_hold;
static int s_ff_toggle;

void ace_ff_hold(int on) { s_ff_hold = on != 0; }
void ace_ff_toggle(void) { s_ff_toggle = !s_ff_toggle; }
int ace_ff_requested(void) { return s_ff_hold || s_ff_toggle; }
