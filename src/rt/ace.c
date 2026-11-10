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
   nothing: tier is the one key a line may leave out (an Enhanced run). */
typedef enum {
    ACE_KEY_AF, ACE_KEY_TEXT, ACE_KEY_READOUT, ACE_KEY_NOTICE, ACE_KEY_WIDE, ACE_KEY_SMOOTH,
    ACE_KEY_SAS, ACE_KEY_TESS, ACE_KEY_TIER, ACE_KEY_PAD, ACE_KEY_CULL
} AceKey;

static const struct {
    const char *name;
    AceKey key;
    int required;
} k_ace_keys[] = {
    {"af", ACE_KEY_AF, 1},
    {"text", ACE_KEY_TEXT, 1},
    {"readout", ACE_KEY_READOUT, 1},
    {"notice", ACE_KEY_NOTICE, 1},
    {"wide", ACE_KEY_WIDE, 1},
    {"smooth", ACE_KEY_SMOOTH, 1},
    {"sas", ACE_KEY_SAS, 1},
    {"tess", ACE_KEY_TESS, 1},
    {"tier", ACE_KEY_TIER, 0},
    {"pad", ACE_KEY_PAD, 0},
    {"cull", ACE_KEY_CULL, 0},
};

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
            case ACE_KEY_PAD:
                if (strcmp(v, "off") == 0) { c.pad = 0; ok = 1; }
                else if (strcmp(v, "late") == 0) { c.pad = 1; ok = 1; }
                if (ok && c.pad && !ACE_HAS_PAD) {
                    why_set(why, why_cap, "pad is not part of this build"); return 0;
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
    /* Late pad sampling acts at a title package's update boundary (contract 11.3.9): Ace only. */
    if (c.pad && c.tier != ACE_TIER_ACE) { why_set(why, why_cap, "pad needs tier=ace"); return 0; }
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
                                "sas=%s;tess=%s",
                      tier_word(c->tier), c->af, on_off(c->text), on_off(c->readout),
                      on_off(c->notice), wide_word(c->wide), smooth_word(c->smooth),
                      sas_word(c->sas), tess_word(c->tess));
    /* Written only when on: a missing pad or cull is off, so every line without it keeps its
     * text. They follow the parts' order, cull last. */
    if (n > 0 && (size_t)n < cap && c && c->on && c->pad) {
        const int m = snprintf(out + n, cap - (size_t)n, ";pad=late");
        n = m < 0 ? -1 : n + m;
    }
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
static uint64_t s_af_failures;
static unsigned long s_cull_untracked;
static AcePadPlan s_pad_plan;
static AceLatency s_latency;

static const char *const k_feature_key[ACE_FEATURE_COUNT] = {
    "af", "text", "readout", "notice", "wide", "smooth", "sas", "tess", "pad", "cull"
};

/* The run's observation state. */
static AceDetector s_det;
static AceReadoutState s_readout;
static AceReadoutView s_view;
static int s_have_view;
static AceCounters s_counters;
static uint64_t s_win_start_ns;
static int s_win_started;
static uint32_t s_win_frames, s_win_skipped, s_win_betweens, s_win_shown;
static uint64_t s_shown_seq;   /* AcePace.frame_seq of the newest real frame that reached the screen */
static uint64_t s_win_idle_ns;
static uint32_t s_last_flip_vcount, s_last_flip_fb;
static int s_have_flip;
static uint64_t s_rate_vblanks[ACE_RATE_COUNT];
static uint64_t s_limited_windows;
static AcePace s_pace;
static AcePaceStats s_pace_stats;
static uint32_t s_pace_vcount;
static AceSmoothAuxStats s_aux_stats;
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
    s_af_failures = 0;
    s_cull_untracked = 0;
    ace_pad_plan_reset(&s_pad_plan);
    ace_latency_reset(&s_latency);
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
            "smooth=%s, sas=%s, tess=%s)\n", tier_word(s_req.tier), s_req.af, on_off(s_req.text),
            on_off(s_req.readout), on_off(s_req.notice), wide_word(s_req.wide),
            smooth_word(s_req.smooth), sas_word(s_req.sas), tess_word(s_req.tess));
    if (s_req.cull) fprintf(stderr, "ACE_MODE: cull on (a title's own culling is widened to the wings)\n");
    if (s_req.pad)
        fprintf(stderr, "ACE_MODE: pad late (each logic update starts closer to its VBLANK deadline "
                        "and reads a fresher pad sample)\n");
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
    case ACE_FEATURE_CULL: s_eff.cull = 0; break;
    case ACE_FEATURE_SAS: s_eff.sas = 0; break;
    case ACE_FEATURE_PAD: s_eff.pad = 0; break;
    default: s_eff.tess = 0; break;
    }
    fprintf(stderr, "ACE_MODE: %s unavailable (%s)\n", k_feature_key[feature], s_unavailable[feature]);
}

void ace_set_cull_untracked(unsigned long n) {
    if (g_ace_on) s_cull_untracked = n;
}

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
    if (s_eff.pad) used = words_add(out, cap, used, "late pad sampling");
    if (s_eff.cull) used = words_add(out, cap, used, "wider culling");
    if (s_eff.smooth) used = words_add(out, cap, used, "smooth motion");
    if (s_eff.sas) used = words_add(out, cap, used, "clean pitched sound");
    if (s_eff.tess) {
        char tess[32];
        snprintf(tess, sizeof(tess), "%dx patches", s_eff.tess);
        used = words_add(out, cap, used, tess);
    }
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
        size_t len = strlen(v.text);
        if (w->betweens)
            snprintf(v.text + len, sizeof(v.text) - len, "  SMOOTH %d", (int)(v.shown_fps + 0.5));
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

void ace_note_skipped_slots(uint32_t slots) {
    if (!g_ace_on || !slots) return;
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
    if (s_pace.held && new_frame) s_pace_stats.superseded++;
    ace_pace_flip(&s_pace, new_frame, s_det.stable, s_eff.smooth, now_ns);
    if (!s_win_started) {
        s_win_started = 1;
        s_win_start_ns = now_ns;
        s_win_frames = 0; s_win_skipped = 0; s_win_betweens = 0; s_win_shown = 0; s_win_idle_ns = 0;
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
        ace_readout_judge(&s_readout, &w, &s_view);
        s_have_view = 1;
        s_counters.windows++;
        if (s_view.pc_limited) s_limited_windows++;
        s_win_start_ns = now_ns;
        s_win_frames = 0; s_win_skipped = 0; s_win_betweens = 0; s_win_shown = 0; s_win_idle_ns = 0;
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
    p->reflip = p->held && !new_frame;
    p->held = 0;
    p->flip_ns = now_ns;
    if (smooth && new_frame) p->frame_seq++;
    if (!smooth || !new_frame) p->show = ACE_SHOW_REAL;
    else p->show = rate == ACE_RATE_K2 ? ACE_SHOW_BETWEEN : ACE_SHOW_KEEP;
}

AceShow ace_pace_take(AcePace *p) {
    AceShow show = p->show;
    p->show = ACE_SHOW_REAL;
    if (show == ACE_SHOW_REAL) return show;
    if (show == ACE_SHOW_BETWEEN && p->taken_seq + 1u != p->frame_seq) {
        show = ACE_SHOW_KEEP;
        p->unpaired++;
    }
    p->taken_seq = p->frame_seq;
    return show;
}

void ace_pace_shown_between(AcePace *p, uint32_t vcount, uint64_t now_ns) {
    p->held = 1;
    p->held_after = vcount;
    p->held_ns = now_ns;
}

AcePresentKind ace_pace_flip_kind(const AcePace *p) {
    return p->reflip ? ACE_PRESENT_HELD : ACE_PRESENT_REAL;
}

int ace_pace_vblank(AcePace *p, uint32_t vcount) {
    if (!p->held || vcount == p->held_after) return 0;
    p->held = 0;
    return 1;
}

AceShow ace_smooth_take(void) {
    return g_ace_on ? ace_pace_take(&s_pace) : ACE_SHOW_REAL;
}

void ace_smooth_shown_between(uint64_t now_ns) {
    if (!g_ace_on) return;
    ace_pace_shown_between(&s_pace, s_pace_vcount, now_ns);
}

int ace_smooth_vblank(uint32_t vcount) {
    return g_ace_on && ace_pace_vblank(&s_pace, vcount);
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
    if (kind == ACE_PRESENT_HELD && now_ns > s_pace.held_ns) {
        uint64_t ns = now_ns - s_pace.held_ns;
        s_pace_stats.hold_count++;
        s_pace_stats.hold_sum_ns += ns;
        if (ns > s_pace_stats.hold_max_ns) s_pace_stats.hold_max_ns = ns;
    }
}

void ace_pace_stats(AcePaceStats *out) {
    if (out) *out = s_pace_stats;
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

/* ---- late pad sampling --------------------------------------------------------------------- */

void ace_pad_plan_reset(AcePadPlan *p) {
    if (p) memset(p, 0, sizeof(*p));
}

uint64_t ace_pad_plan_begin(AcePadPlan *p, uint64_t now_ns, uint64_t deadline_ns, uint64_t period_ns,
                            int allow) {
    if (!p) return 0;
    if (p->open) {             /* the previous update never waited for a VBLANK */
        p->unmeasured++;
        p->warm = 0;
    }
    p->updates++;
    p->open = 1;
    p->held = 0;
    p->release_ns = now_ns;
    p->deadline_ns = deadline_ns;
    /* A deadline more than one period ahead is not the next VBLANK on this clock (with no
     * period known, every deadline counts as one): no hold, the update is not judged late
     * against it, and the plan warms up again before it holds. */
    const int too_far = deadline_ns && deadline_ns > now_ns && (!period_ns || deadline_ns - now_ns > period_ns);
    if (too_far) {
        p->far_deadlines++;
        p->warm = 0;
        p->deadline_ns = 0;
    }
    if (p->backoff) {
        p->backoff--;
        return 0;
    }
    if (too_far || !allow || !deadline_ns || p->warm < ACE_PAD_WARM || !p->ncost) return 0;
    uint64_t work = 0;
    for (uint32_t i = 0; i < p->ncost; i++)
        if (p->cost[i] > work) work = p->cost[i];
    const uint64_t reserve = work + work / 4u + ACE_PAD_MARGIN_NS + p->extra_margin_ns;
    if (deadline_ns <= now_ns || deadline_ns - now_ns <= reserve + ACE_PAD_MIN_HOLD_NS) return 0;
    /* The deadline is at most one period ahead here, so every hold is shorter than a period. */
    const uint64_t hold = deadline_ns - now_ns - reserve;
    p->held = 1;
    p->held_updates++;
    p->hold_ns_sum += hold;
    if (hold > p->hold_ns_max) p->hold_ns_max = hold;
    return hold;
}

void ace_pad_plan_release(AcePadPlan *p, uint64_t now_ns) {
    if (p && p->open) p->release_ns = now_ns;
}

int ace_pad_plan_wait(AcePadPlan *p, uint64_t now_ns) {
    if (!p || !p->open) return 0;
    p->open = 0;
    p->measured++;
    p->cost[p->next] = now_ns > p->release_ns ? now_ns - p->release_ns : 0u;
    p->next = (p->next + 1u) % ACE_PAD_COSTS;
    if (p->ncost < ACE_PAD_COSTS) p->ncost++;
    if (p->warm < ACE_PAD_WARM) p->warm++;
    const int late = p->deadline_ns && now_ns > p->deadline_ns;
    if (!late) return 0;
    if (!p->held) {
        p->late_unheld++;
        return 0;
    }
    p->late_held++;
    p->backoff = ACE_PAD_BACKOFF_UPDATES;
    p->extra_margin_ns += ACE_PAD_MARGIN_STEP_NS;
    if (p->extra_margin_ns > ACE_PAD_MARGIN_MAX_NS) p->extra_margin_ns = ACE_PAD_MARGIN_MAX_NS;
    return 1;
}

AcePadPlan *ace_pad_plan(void) { return &s_pad_plan; }

void ace_latency_reset(AceLatency *l) {
    if (l) memset(l, 0, sizeof(*l));
}

void ace_latency_note(AceLatency *l, uint64_t ns) {
    if (!l) return;
    uint64_t b = ns / ACE_LAT_BIN_NS;
    if (b >= ACE_LAT_BINS) b = ACE_LAT_BINS - 1u;
    l->bin[b]++;
    l->count++;
    l->sum_ns += ns;
    if (ns > l->max_ns) l->max_ns = ns;
}

uint64_t ace_latency_percentile_ns(const AceLatency *l, unsigned pct) {
    if (!l || !l->count) return 0;
    if (pct > 100u) pct = 100u;
    /* The upper edge of the first bin by which at least pct percent of the frames are counted. */
    const uint64_t want = (l->count * pct + 99u) / 100u;
    uint64_t seen = 0;
    for (uint32_t b = 0; b < ACE_LAT_BINS; b++) {
        seen += l->bin[b];
        if (seen && seen >= want) return (uint64_t)(b + 1u) * ACE_LAT_BIN_NS;
    }
    return (uint64_t)ACE_LAT_BINS * ACE_LAT_BIN_NS;
}

void ace_note_input_latency(uint64_t ns) {
    if (g_ace_on) ace_latency_note(&s_latency, ns);
}

void ace_note_frame_without_read(void) {
    if (g_ace_on) s_latency.without_read++;
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
            "\"wide\": \"%s\", \"smooth\": \"%s\", \"sas\": \"%s\", \"tess\": \"%s\"},\n",
            s_eff.af, s_eff.text ? "true" : "false", s_eff.readout ? "true" : "false",
            s_eff.notice ? "true" : "false", wide_word(s_eff.wide), smooth_word(s_eff.smooth),
            sas_word(s_eff.sas), tess_word(s_eff.tess));
    if (s_req.pad || s_pad_plan.updates) {
        const AcePadPlan *pp = &s_pad_plan;
        ACE_PUT("  \"pad\": {\"late\": %s, \"updates\": %llu, \"measured\": %llu, \"unmeasured\": %llu, "
                "\"held\": %llu, \"hold_mean_ms\": %.2f, \"hold_max_ms\": %.2f, \"late_held\": %llu, "
                "\"late_unheld\": %llu, \"refreshed\": %llu, \"far_deadline\": %llu},\n",
                s_eff.pad ? "true" : "false", (unsigned long long)pp->updates,
                (unsigned long long)pp->measured, (unsigned long long)pp->unmeasured,
                (unsigned long long)pp->held_updates,
                pp->held_updates ? (double)pp->hold_ns_sum / (double)pp->held_updates / 1e6 : 0.0,
                (double)pp->hold_ns_max / 1e6, (unsigned long long)pp->late_held,
                (unsigned long long)pp->late_unheld, (unsigned long long)pp->refreshed,
                (unsigned long long)pp->far_deadlines);
    }
    if (s_req.cull) ACE_PUT("  \"cull\": %s,\n  \"cull_untracked\": %lu,\n",
                            s_eff.cull ? "true" : "false", s_cull_untracked);
    if (s_latency.count || s_latency.without_read)
        ACE_PUT("  \"input_latency_ms\": {\"frames\": %llu, \"frames_without_read\": %llu, \"mean\": %.2f, "
                "\"p50\": %.1f, \"p95\": %.1f, \"p99\": %.1f, \"max\": %.2f},\n",
                (unsigned long long)s_latency.count, (unsigned long long)s_latency.without_read,
                s_latency.count ? (double)s_latency.sum_ns / (double)s_latency.count / 1e6 : 0.0,
                (double)ace_latency_percentile_ns(&s_latency, 50) / 1e6,
                (double)ace_latency_percentile_ns(&s_latency, 95) / 1e6,
                (double)ace_latency_percentile_ns(&s_latency, 99) / 1e6, (double)s_latency.max_ns / 1e6);
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
                "\"superseded\": %llu, \"unpaired\": %llu},\n",
                (unsigned long long)ps->kind[ACE_PRESENT_REAL],
                (unsigned long long)ps->kind[ACE_PRESENT_BETWEEN],
                (unsigned long long)ps->kind[ACE_PRESENT_HELD],
                (unsigned long long)ps->superseded, (unsigned long long)s_pace.unpaired);
        ACE_PUT("  \"added_delay_ms\": {\"mean\": %.2f, \"max\": %.2f},\n",
                ps->hold_count ? (double)ps->hold_sum_ns / (double)ps->hold_count / 1e6 : 0.0,
                (double)ps->hold_max_ns / 1e6);
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

void ace_finish(void) {
    char json[ACE_REPORT_MAX];
    if (!g_ace_on) return;
    /* A feature that is on but never ran: its renderer or presenter did not start. */
    static const int has[ACE_FEATURE_COUNT] = { 1, 1, 1, 1, ACE_HAS_WIDE, ACE_HAS_SMOOTH, ACE_HAS_SAS, ACE_HAS_TESS,
                                                ACE_HAS_PAD, ACE_HAS_CULL };
    const int on[ACE_FEATURE_COUNT] = { s_eff.af > 0, s_eff.text, s_eff.readout, s_eff.notice,
                                        s_eff.wide, s_eff.smooth, s_eff.sas, s_eff.tess, s_eff.pad,
                                        s_eff.cull };
    for (int f = 0; f < ACE_FEATURE_COUNT; f++) {
        if (has[f] && on[f] && !s_confirmed[f])
            ace_feature_unavailable((AceFeature)f, f == ACE_FEATURE_CULL
                ? "did not run: this title has no culling package in its build, or Widescreen is off"
                : f == ACE_FEATURE_PAD
                ? "did not run: no update boundary ran (no 60 FPS game logic package, or it is off)"
                : "did not run: its renderer or presenter did not start");
    }
    size_t len = ace_report_json(json, sizeof(json));
    fprintf(stderr, "ACE_MODE: exit (%s; rate %s)\n", s_spec, ace_rate_name(dominant_rate()));
    if (s_latency.count || s_latency.without_read)
        fprintf(stderr, "ACE_MODE: input latency (%llu frames: mean %.2f ms, p50 %.1f ms, p95 %.1f ms, "
                        "max %.2f ms; %llu frames set with no pad read since the previous present)\n",
                (unsigned long long)s_latency.count,
                s_latency.count ? (double)s_latency.sum_ns / (double)s_latency.count / 1e6 : 0.0,
                (double)ace_latency_percentile_ns(&s_latency, 50) / 1e6,
                (double)ace_latency_percentile_ns(&s_latency, 95) / 1e6, (double)s_latency.max_ns / 1e6,
                (unsigned long long)s_latency.without_read);
    if (s_pad_plan.updates)
        fprintf(stderr, "ACE_MODE: pad %s (%llu updates: %llu held, mean hold %.2f ms; %llu unmeasured; "
                        "late after a hold %llu, late without one %llu; %llu samples refreshed)\n",
                s_eff.pad ? "late" : "off", (unsigned long long)s_pad_plan.updates,
                (unsigned long long)s_pad_plan.held_updates,
                s_pad_plan.held_updates ? (double)s_pad_plan.hold_ns_sum / (double)s_pad_plan.held_updates / 1e6
                                        : 0.0,
                (unsigned long long)s_pad_plan.unmeasured, (unsigned long long)s_pad_plan.late_held,
                (unsigned long long)s_pad_plan.late_unheld, (unsigned long long)s_pad_plan.refreshed);
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
