/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The launcher's hidden moment (launcher_secret.h): the code's detector, the greeting, the
   moment's timeline, the rippled mark, the confetti and the jingle. Project-written; the
   ripple, the confetti and the jingle are computed here, with no picture or sound file. */

#include "launcher_secret.h"

#include <math.h>
#include <string.h>

#define LAUNCHER_PI 3.14159265358979323846

/* ---- The code ------------------------------------------------------------------------------ */

static const LauncherSecretKey g_launcher_code[LAUNCHER_SECRET_STEPS] = {
    LAUNCHER_SECRET_KEY_UP, LAUNCHER_SECRET_KEY_UP, LAUNCHER_SECRET_KEY_DOWN,
    LAUNCHER_SECRET_KEY_DOWN, LAUNCHER_SECRET_KEY_LEFT, LAUNCHER_SECRET_KEY_RIGHT,
    LAUNCHER_SECRET_KEY_LEFT, LAUNCHER_SECRET_KEY_RIGHT, LAUNCHER_SECRET_KEY_B,
    LAUNCHER_SECRET_KEY_A
};

LauncherSecretKey launcher_secret_key_for_event(const NkUiEvent *event) {
    if (event == NULL) return LAUNCHER_SECRET_KEY_NONE;
    switch (event->type) {
        case NK_UI_EV_POINTER:
        case NK_UI_EV_POINTER_LEAVE:
        case NK_UI_EV_WHEEL:
        case NK_UI_EV_DEVICE:
        case NK_UI_EV_TEXT:
            return LAUNCHER_SECRET_KEY_NONE;
        default:
            break;
    }
    if (!event->down || event->repeat) return LAUNCHER_SECRET_KEY_NONE;
    switch (event->type) {
        case NK_UI_EV_NAV:
            switch (event->dir) {
                case NK_UI_DIR_UP: return LAUNCHER_SECRET_KEY_UP;
                case NK_UI_DIR_DOWN: return LAUNCHER_SECRET_KEY_DOWN;
                case NK_UI_DIR_LEFT: return LAUNCHER_SECRET_KEY_LEFT;
                case NK_UI_DIR_RIGHT: return LAUNCHER_SECRET_KEY_RIGHT;
                default: return LAUNCHER_SECRET_KEY_OTHER;
            }
        case NK_UI_EV_BACK: return LAUNCHER_SECRET_KEY_B;
        case NK_UI_EV_ACTIVATE: return LAUNCHER_SECRET_KEY_A;
        default: return LAUNCHER_SECRET_KEY_OTHER;
    }
}

void launcher_secret_reset(LauncherSecretDetector *detector) {
    if (detector != NULL) memset(detector, 0, sizeof(*detector));
}

/* The longest run of the newest presses that is the start of the code. */
static size_t launcher_secret_matched(const LauncherSecretDetector *detector) {
    size_t longest = detector->count < LAUNCHER_SECRET_STEPS ? detector->count
                                                             : LAUNCHER_SECRET_STEPS;
    for (size_t k = longest; k > 0u; --k) {
        const LauncherSecretKey *tail = &detector->keys[detector->count - k];
        if (memcmp(tail, g_launcher_code, k * sizeof(g_launcher_code[0])) == 0) return k;
    }
    return 0u;
}

size_t launcher_secret_progress(const LauncherSecretDetector *detector) {
    return detector != NULL ? launcher_secret_matched(detector) : 0u;
}

LauncherSecretVerdict launcher_secret_feed(LauncherSecretDetector *detector, LauncherSecretKey key,
                                           double now_s) {
    size_t matched;
    if (detector == NULL || key == LAUNCHER_SECRET_KEY_NONE) return LAUNCHER_SECRET_PASS;
    if (detector->count > 0u && !(now_s - detector->last_s <= LAUNCHER_SECRET_GAP_S)) {
        detector->count = 0u;
    }
    if (detector->count == LAUNCHER_SECRET_STEPS) {
        memmove(detector->keys, detector->keys + 1, (LAUNCHER_SECRET_STEPS - 1u) *
                                                        sizeof(detector->keys[0]));
        --detector->count;
    }
    detector->keys[detector->count++] = key;
    detector->last_s = now_s;
    matched = launcher_secret_matched(detector);
    if (matched == LAUNCHER_SECRET_STEPS) {
        launcher_secret_reset(detector);
        return LAUNCHER_SECRET_FOUND;
    }
    if (key == LAUNCHER_SECRET_KEY_B && matched == LAUNCHER_SECRET_STEPS - 1u) {
        return LAUNCHER_SECRET_SWALLOW;
    }
    return LAUNCHER_SECRET_PASS;
}

/* ---- The words ------------------------------------------------------------------------------ */

const char *launcher_secret_greeting(int hour, int minute) {
    (void)minute; /* every boundary falls on the hour */
    if (hour >= 5 && hour < 12) return NK_UI_STR(ADAPTER_GREETING_MORNING);
    if (hour >= 12 && hour < 18) return NK_UI_STR(ADAPTER_GREETING_AFTERNOON);
    return NK_UI_STR(ADAPTER_GREETING_EVENING);
}

/* ---- The moment ----------------------------------------------------------------------------- */

void launcher_moment_start(LauncherMoment *moment, LauncherMomentKind kind, double now_s,
                           bool reduce_motion, bool ui_sounds, const char *toast) {
    if (moment == NULL) return;
    moment->kind = kind;
    moment->start_s = now_s;
    moment->motion = !reduce_motion;
    moment->sound = ui_sounds && kind == LAUNCHER_MOMENT_FOUND;
    moment->toast = toast;
}

static float launcher_ease(double x) {
    if (x <= 0.0) return 0.0f;
    if (x >= 1.0) return 1.0f;
    return (float)(x * x * (3.0 - 2.0 * x));
}

LauncherMomentFrame launcher_moment_frame(const LauncherMoment *moment, double prev_s,
                                          double now_s) {
    LauncherMomentFrame frame;
    double t;
    double toast_s;
    double toast_from;
    memset(&frame, 0, sizeof(frame));
    if (moment == NULL || moment->kind == LAUNCHER_MOMENT_NONE) return frame;
    t = now_s - moment->start_s;
    if (t < 0.0) t = 0.0;
    toast_s = moment->kind == LAUNCHER_MOMENT_FOUND ? LAUNCHER_MOMENT_TOAST_S
                                                    : LAUNCHER_MOMENT_FAREWELL_S;
    frame.active = t < toast_s;
    if (moment->kind == LAUNCHER_MOMENT_FOUND) {
        double handover = moment->motion ? LAUNCHER_MOMENT_RIPPLE_S : 0.0;
        double at = moment->start_s + handover;
        frame.open_love_all = prev_s < at && now_s >= at;
        if (moment->motion && t < LAUNCHER_MOMENT_RIPPLE_S) {
            frame.ripple = true;
            frame.ripple_t = (float)t;
            frame.ripple_strength = launcher_ease(t / 0.3) *
                                    launcher_ease((LAUNCHER_MOMENT_RIPPLE_S - t) / 0.45);
        }
        if (moment->motion && t < LAUNCHER_MOMENT_CONFETTI_S) {
            frame.confetti = true;
            frame.confetti_t = (float)t;
        }
    }
    toast_from = moment->motion && moment->kind == LAUNCHER_MOMENT_FOUND ? 0.15 : 0.0;
    if (frame.active && t >= toast_from && moment->toast != NULL) {
        frame.toast = true;
        if (moment->motion) {
            float in = launcher_ease((t - toast_from) / LAUNCHER_MOMENT_FADE_S);
            float out = launcher_ease((toast_s - t) / LAUNCHER_MOMENT_FADE_S);
            frame.toast_alpha = in < out ? in : out;
            frame.toast_rise = 16.0f * (1.0f - in);
        } else {
            frame.toast_alpha = 1.0f;
        }
    }
    return frame;
}

/* ---- The rippled mark ---------------------------------------------------------------------- */

#define RIPPLE_AMPLITUDE 11.0   /* view units: the mark is 172 wide */
#define RIPPLE_WAVELENGTH 118.0
#define RIPPLE_SPEED_HZ 1.35

static double ripple_offset(double x, double origin, float t_s, float strength) {
    double phase = 2.0 * LAUNCHER_PI * ((x - origin) / RIPPLE_WAVELENGTH - RIPPLE_SPEED_HZ * t_s);
    return RIPPLE_AMPLITUDE * (double)strength * sin(phase);
}

/* Clips a convex polygon to x >= edge (keep_right) or x <= edge; returns the points left. */
static unsigned ripple_clip(const float *in, unsigned count, float edge, bool keep_right,
                            float *out) {
    unsigned written = 0u;
    for (unsigned i = 0u; i < count; ++i) {
        const float *a = &in[2u * i];
        const float *b = &in[2u * ((i + 1u) % count)];
        bool a_in = keep_right ? a[0] >= edge : a[0] <= edge;
        bool b_in = keep_right ? b[0] >= edge : b[0] <= edge;
        if (a_in) {
            out[2u * written] = a[0];
            out[2u * written + 1u] = a[1];
            ++written;
        }
        if (a_in != b_in && b[0] != a[0]) {
            float k = (edge - a[0]) / (b[0] - a[0]);
            out[2u * written] = edge;
            out[2u * written + 1u] = a[1] + (b[1] - a[1]) * k;
            ++written;
        }
    }
    return written;
}

size_t launcher_ripple_shapes(float t_s, float strength, NkUiShape *out, size_t cap) {
    const NkUiShapeArt *mark = nk_ui_art_n1_mark();
    float strip_w;
    size_t written = 0u;
    if (out == NULL || mark == NULL) return 0u;
    strip_w = mark->view_w / (float)LAUNCHER_RIPPLE_STRIPS;
    for (size_t s = 0u; s < mark->count; ++s) {
        const NkUiShape *shape = &mark->shapes[s];
        float poly[2u * NK_UI_SHAPE_MAX_POINTS];
        unsigned points = 0u;
        if (shape->kind == NK_UI_SHAPE_RECT) {
            const float x = shape->v[0], y = shape->v[1], w = shape->v[2], h = shape->v[3];
            const float corners[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
            memcpy(poly, corners, sizeof(corners));
            points = 4u;
        } else if (shape->kind == NK_UI_SHAPE_POLY && shape->count >= 3u &&
                   shape->count <= NK_UI_SHAPE_MAX_POINTS) {
            memcpy(poly, shape->v, 2u * shape->count * sizeof(float));
            points = shape->count;
        } else {
            continue;
        }
        for (unsigned strip = 0u; strip < LAUNCHER_RIPPLE_STRIPS; ++strip) {
            const float x0 = mark->view_x + strip_w * (float)strip;
            const float x1 = x0 + strip_w;
            /* Room for two more points per clip edge, as NK_UI_SHAPE_MAX_POINTS + 2. */
            float left[2u * (NK_UI_SHAPE_MAX_POINTS + 2u)];
            float both[2u * (NK_UI_SHAPE_MAX_POINTS + 4u)];
            unsigned n = ripple_clip(poly, points, x0, true, left);
            double d0;
            double d1;
            NkUiShape *piece;
            if (n < 3u) continue;
            n = ripple_clip(left, n, x1, false, both);
            if (n < 3u || n > NK_UI_SHAPE_MAX_POINTS) continue;
            if (written == cap) return written;
            /* The strip is sheared, not bent: every point moves by the line between the
               wave's heights at the strip's two edges, so the piece stays convex. */
            d0 = ripple_offset(x0, mark->view_x, t_s, strength);
            d1 = ripple_offset(x1, mark->view_x, t_s, strength);
            piece = &out[written++];
            memset(piece, 0, sizeof(*piece));
            piece->kind = NK_UI_SHAPE_POLY;
            piece->rgba = shape->rgba;
            piece->count = n;
            for (unsigned p = 0u; p < n; ++p) {
                double k = (both[2u * p] - x0) / strip_w;
                piece->v[2u * p] = both[2u * p];
                piece->v[2u * p + 1u] = (float)(both[2u * p + 1u] + d0 + (d1 - d0) * k);
            }
        }
    }
    return written;
}

/* ---- Confetti -------------------------------------------------------------------------------- */

static uint32_t confetti_next(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static float confetti_unit(uint32_t *state) {
    return (float)(confetti_next(state) & 0xFFFFu) / 65535.0f;
}

size_t launcher_confetti_shapes(float t_s, float w, float h, const uint32_t *colors,
                                size_t color_count, NkUiShape *out, size_t cap) {
    uint32_t seed = 0x4E414D49u; /* any fixed seed: the same fall every run */
    size_t written = 0u;
    float fade = 1.0f;
    if (out == NULL || colors == NULL || color_count == 0u || !(w > 0.0f) || !(h > 0.0f)) {
        return 0u;
    }
    if (t_s > (float)LAUNCHER_MOMENT_CONFETTI_S - 0.6f) {
        fade = ((float)LAUNCHER_MOMENT_CONFETTI_S - t_s) / 0.6f;
        if (fade < 0.0f) fade = 0.0f;
    }
    for (size_t i = 0u; i < LAUNCHER_CONFETTI_PIECES && written < cap; ++i) {
        float x0 = confetti_unit(&seed) * w;
        float y0 = -24.0f - confetti_unit(&seed) * h * 0.35f;
        float fall = h * (0.30f + 0.22f * confetti_unit(&seed));
        float sway = 10.0f + 18.0f * confetti_unit(&seed);
        float sway_hz = 0.6f + 0.9f * confetti_unit(&seed);
        float spin = (confetti_unit(&seed) - 0.5f) * 9.0f;
        float angle0 = confetti_unit(&seed) * 6.2831853f;
        float half_w = 3.0f + 2.5f * confetti_unit(&seed);
        float half_h = 1.5f + 1.2f * confetti_unit(&seed);
        uint32_t rgba = colors[confetti_next(&seed) % color_count];
        float cx = x0 + sway * sinf(6.2831853f * sway_hz * t_s + angle0);
        float cy = y0 + fall * t_s + 0.5f * 90.0f * t_s * t_s;
        float angle = angle0 + spin * t_s;
        /* The turn foreshortens the piece, as paper turning over. */
        float flip = 0.35f + 0.65f * fabsf(cosf(angle * 1.7f));
        float c = cosf(angle);
        float s = sinf(angle);
        NkUiShape *piece;
        if (cy < -40.0f || cy > h + 20.0f) continue;
        piece = &out[written++];
        memset(piece, 0, sizeof(*piece));
        piece->kind = NK_UI_SHAPE_POLY;
        piece->rgba = (rgba & 0xFFFFFF00u) |
                      (uint32_t)lroundf((float)(rgba & 0xFFu) * fade);
        piece->count = 4u;
        {
            const float corners[4][2] = {{-half_w, -half_h * flip}, {half_w, -half_h * flip},
                                         {half_w, half_h * flip}, {-half_w, half_h * flip}};
            for (unsigned p = 0u; p < 4u; ++p) {
                piece->v[2u * p] = cx + corners[p][0] * c - corners[p][1] * s;
                piece->v[2u * p + 1u] = cy + corners[p][0] * s + corners[p][1] * c;
            }
        }
    }
    return written;
}

/* ---- The jingle ----------------------------------------------------------------------------- */

typedef struct {
    double hz;
    double seconds;
} JingleNote;

/* A rising arpeggio that turns and lifts once more, like a wave's crest: project-written. */
static const JingleNote g_jingle_lead[] = {
    {523.25, 0.075}, {659.26, 0.075}, {783.99, 0.075}, {1046.50, 0.100},
    {880.00, 0.075}, {987.77, 0.075}, {1318.51, 0.340}
};
static const JingleNote g_jingle_bass[] = {
    {130.81, 0.300}, {196.00, 0.175}, {261.63, 0.340}
};

static double jingle_total_s(const JingleNote *notes, size_t count) {
    double total = 0.0;
    for (size_t i = 0u; i < count; ++i) total += notes[i].seconds;
    return total;
}

/* The note sounding at t, its start and length; false past the end. */
static bool jingle_at(const JingleNote *notes, size_t count, double t, const JingleNote **note,
                      double *start) {
    double at = 0.0;
    for (size_t i = 0u; i < count; ++i) {
        if (t < at + notes[i].seconds) {
            *note = &notes[i];
            *start = at;
            return true;
        }
        at += notes[i].seconds;
    }
    return false;
}

/* A short attack, a gentle decay and a release at the note's end, so no note clicks. */
static double jingle_envelope(double since, double length) {
    double attack = since < 0.004 ? since / 0.004 : 1.0;
    double release = length - since < 0.012 ? (length - since) / 0.012 : 1.0;
    double decay = 0.62 + 0.38 * exp(-since * 9.0);
    if (release < 0.0) release = 0.0;
    return attack * release * decay;
}

size_t launcher_jingle_pcm(int16_t *out, size_t cap, int rate) {
    const size_t lead_count = sizeof(g_jingle_lead) / sizeof(g_jingle_lead[0]);
    const size_t bass_count = sizeof(g_jingle_bass) / sizeof(g_jingle_bass[0]);
    double total;
    size_t samples;
    double lead_phase = 0.0;
    double bass_phase = 0.0;
    if (rate < 8000 || rate > 192000) return 0u;
    total = jingle_total_s(g_jingle_lead, lead_count);
    samples = (size_t)(total * (double)rate);
    if (out == NULL) return samples;
    for (size_t i = 0u; i < samples && i < cap; ++i) {
        double t = (double)i / (double)rate;
        const JingleNote *note = NULL;
        double start = 0.0;
        double value = 0.0;
        if (jingle_at(g_jingle_lead, lead_count, t, &note, &start)) {
            lead_phase += note->hz / (double)rate;
            lead_phase -= floor(lead_phase);
            /* A 25 % pulse, the chiptune lead. */
            value += (lead_phase < 0.25 ? 1.0 : -1.0) * 0.20 *
                     jingle_envelope(t - start, note->seconds);
        }
        if (jingle_at(g_jingle_bass, bass_count, t, &note, &start)) {
            double tri;
            bass_phase += note->hz / (double)rate;
            bass_phase -= floor(bass_phase);
            tri = 4.0 * fabs(bass_phase - 0.5) - 1.0;
            value += tri * 0.18 * jingle_envelope(t - start, note->seconds);
        }
        /* The last 80 ms fade out. */
        if (total - t < 0.08) value *= (total - t) / 0.08;
        if (value > 1.0) value = 1.0;
        if (value < -1.0) value = -1.0;
        out[i] = (int16_t)lround(value * 32767.0);
    }
    return samples;
}
