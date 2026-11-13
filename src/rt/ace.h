/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace.h - the Enhanced and Ace play modes' runtime side (docs/ENHANCEMENT_CONTRACT.md section
 * 10). The internal name stays "ace"; the launcher calls the modes Enhanced and Ace (#2055).
 *
 * One launch variable carries the run's choice. The launcher always exports it, so an inherited
 * value can never opt a launch in:
 *
 *     SR_ACE=off
 *     SR_ACE=v1;tier=enhanced;af=16;text=on;readout=off;notice=on;wide=off;smooth=off;sas=off;tess=off;twin=off
 *
 * tier (enhanced or ace) only names the run, in the badge, the pause menu and the stamps; a
 * line without it is an Enhanced run. stick, load, depth, ssaa, peak, glyphs and cull are the other
 * keys a line may leave out: a missing stick, load, depth, ssaa, peak, glyphs or cull reads as off
 * (section 11.2 item 2); stick has no off word, its value is the game's dead zone (1..126), and a
 * line that writes load=off, depth=off, ssaa=off, peak=off, glyphs=off or cull=off means the same
 * as none; stick=<counts>, load=fast and cull=on need tier=ace, and depth=32 (the 32-bit host depth
 * buffer, #2051), ssaa=2|4 (supersampling, #2073), peak=limit and glyphs=on (sharp game text,
 * #2110) do not. Every other
 * key is required in v1, in any order, each once. A key or value for a feature this build
 * does not carry (smooth=match) is still parsed and refused. Anything else (an unknown version, key
 * or value, a duplicate, a missing key, an empty field) refuses the whole line: Ace is off for
 * the run and one "ACE_MODE: refused (<reason>)" line says why. Unset is off and says nothing.
 *
 * Off is authentic: every Ace branch in the runtime sits behind g_ace_on, which stays 0, so the
 * Vulkan device, every sampler, the guest shaders, the presents and the observation counters
 * are today's, and nothing is printed. With Ace on, guest logic and the guest timing model are
 * unchanged; host presentation changes (and, with sas=sinc, the mixed SAS sample values; with
 * tess=2..4, the pixels of every triangle patch and the GE timing model's clocks under
 * SR_GE_TIMING=1; with load=fast, the guest timing of a title's declared loading waits; with
 * depth=32, which surface wins where two lie within one 16-bit depth step; with peak=limit, only
 * the host's sound: the guest's SAS bytes stay the PSP's; with glyphs=on at a render scale of 2 to
 * 4, the pixels of small text sprites in the frames the guest keeps), and
 * the run says so ("ACE_MODE: on (...)" at start, an exit line, and the flight recorder's
 * runtime.enhancements block).
 *
 * This unit uses the C library only, and on Windows MoveFileEx for the report's atomic replace.
 * ace-selftest links it with nothing else, so no code path here can reach a guest-visible
 * function (invariant S1). Callers pass the clock in.
 */
#ifndef SR_ACE_H
#define SR_ACE_H

#include <stddef.h>
#include <stdint.h>

#define ACE_SPEC_MAX 192         /* SR_ACE value, terminator included; room for later parts */

/* Features that are part of this build. A feature that is not must be off in SR_ACE. */
#define ACE_HAS_WIDE 1          /* wide=fill: widescreen Fill (#1881), the 3D scene in wings */
#define ACE_HAS_SMOOTH 1        /* smooth=x2: one in-between per 30 Hz frame */
#define ACE_HAS_SMOOTH_MATCH 0  /* smooth=match: the monitor's rate */
#define ACE_HAS_SAS 1           /* sas=sinc: band-limited SAS voice resampling (#2054) */
#define ACE_HAS_TESS 1          /* tess=2..4: finer Bezier and spline patches (#2053) */
#define ACE_HAS_CULL 1          /* cull=on: widen a title's own view-culling test to the wings (11.3.6) */
#define ACE_HAS_SSAA 1          /* ssaa=2|4: supersampling on top of render scale (#2073, 10.21) */
#define ACE_HAS_STICK 1         /* stick=<counts>: start the host stick past the game's dead zone (11.3.11) */
#define ACE_STICK_COUNTS_MAX 126 /* stick=1..126: the square dead zone, in the game's centred counts */
#define ACE_HAS_LOAD 1          /* load=fast: a title's declared loading waits return at once (11.3.10) */
#define ACE_HAS_DEPTH 1         /* depth=32: a 32-bit float host depth buffer (#2051, 10.20, 11.3.12) */
#define ACE_HAS_PEAK 1          /* peak=limit: SAS mixes unsaturated through a true-peak limiter (#2137, 10.22) */
#define ACE_HAS_GLYPHS 1        /* glyphs=on: sharp game text, glyphs rebuilt at the render scale (10.13, 11.3.5) */

typedef enum { ACE_WIDE_OFF = 0, ACE_WIDE_FILL = 1 } AceWide;
typedef enum { ACE_SMOOTH_OFF = 0, ACE_SMOOTH_MATCH = 1, ACE_SMOOTH_X2 = 2 } AceSmooth;
/* Widescreen Fill has no effective output while Smooth motion owns the HUD mask and depth.
 * Keep this rule shared by the presenter and the player's effective-settings row. */
static inline int ace_wide_fill_blocked_by_smooth(int wide_requested, int smooth_requested) {
    return wide_requested != 0 && smooth_requested != 0;
}
/* The play mode a run is: Enhanced, or Ace (Enhanced plus the game's own package). */
typedef enum { ACE_TIER_ENHANCED = 0, ACE_TIER_ACE = 1 } AceTier;
typedef enum { ACE_SAS_OFF = 0, ACE_SAS_SINC = 1 } AceSas;

typedef struct {
    int on;          /* 0: authentic (off, unset or refused); every field below is then 0 */
    int af;          /* texture filtering: 0, 2, 4, 8 or 16 */
    int text;        /* crisp system text: the on-screen keyboard at the window's resolution */
    int readout;     /* the speed readout chip */
    int notice;      /* the launch notice chip */
    int wide;        /* AceWide */
    int smooth;      /* AceSmooth */
    int tier;        /* AceTier */
    int sas;         /* AceSas: SAS voices mix through a band-limited resampler (host-only) */
    int tess;        /* finer patches (#2053): 0 off, or the factor 2, 3 or 4 on the guest's division */
    int cull;        /* cull=on: a title's culling is widened to the wings (guest-visible, 11.3.6) */
    int stick;       /* stick=<counts>: 0 off, else the square stick dead zone the title ignores, 1..126
                        (guest-visible, 11.3.11) */
    int load;        /* load=fast: the waits a title's load part declares are skipped (guest timing, 11.3.10) */
    int depth;       /* depth=32: 0 off (the PSP's 16-bit depth), else 32: the Vulkan renderer's depth
                        buffer holds unrounded depth (guest-visible, 10.20 and 11.3.12) */
    int twin;        /* full 32-bit colour: 16-bit games drawn on a host twin, no dither (host-only) */
    int ssaa;        /* ssaa=2|4: 0 off, else the supersampling factor per axis on top of render scale;
                        host presentation only (10.21) */
    int peak;        /* peak=limit: the host hears SAS mixes unsaturated, through a true-peak limiter
                        (host-only, 10.22) */
    int glyphs;      /* glyphs=on: small text sprites draw from glyphs rebuilt at the render scale
                        (guest-visible, 10.13 and 11.3.5) */
} AceConfig;

typedef enum {
    ACE_FEATURE_AF = 0,
    ACE_FEATURE_TEXT,
    ACE_FEATURE_READOUT,
    ACE_FEATURE_NOTICE,
    ACE_FEATURE_WIDE,
    ACE_FEATURE_SMOOTH,
    ACE_FEATURE_SAS,
    ACE_FEATURE_TESS,
    ACE_FEATURE_STICK,
    ACE_FEATURE_LOAD,
    ACE_FEATURE_CULL,
    ACE_FEATURE_DEPTH,
    ACE_FEATURE_TWIN,
    ACE_FEATURE_SSAA,
    ACE_FEATURE_PEAK,
    ACE_FEATURE_GLYPHS,
    ACE_FEATURE_COUNT
} AceFeature;

/* ---- the launch line ------------------------------------------------------------------------ */

/* Parses one SR_ACE value. Returns 1 for a valid line ("off" fills an all-zero config) and 0
 * for a refusal: then *out is all zero and `why` (when given) names the first problem. NULL is
 * refused as "unset"; ace_init treats unset as off without a refusal line. */
int ace_parse(const char *spec, AceConfig *out, char *why, size_t why_cap);
/* Writes the canonical line for a config ("off" when !c->on). Returns 0 when it does not fit. */
int ace_format(const AceConfig *c, char *out, size_t cap);
/* Writes a line that gives every key of the parser's table its longest value, in the table's
   order, and returns its length (0 when it does not fit). ace_format writes the same length
   for the config it parses to: the longest line there is, whatever keys later parts add. */
size_t ace_longest_line(char *out, size_t cap);

/* ---- the run's switch ----------------------------------------------------------------------- */

extern int g_ace_on;   /* the run's switch: 1 only after a valid line with Ace on */

/* Reads SR_ACE once (and SR_ACE_REPORT when on), prints the start stamp or the refusal, and
 * sets g_ace_on. Returns g_ace_on. ace_init_spec does the same for a given value (tests).
 * A retired variable that is set (ace_retired_env) stops the run first: the named line and exit 1. */
int ace_init_from_env(void);
/* SR_GLYPHS (its value, NULL or "" for unset) once switched sharp game text; the launch line's glyphs
 * key replaced it. Returns 1 and writes the line that names the key to `why` when it is set, else 0. */
int ace_retired_env(const char *glyphs, char *why, size_t why_cap);
int ace_init_spec(const char *spec, const char *report_path);
/* The requested line, and what this host gave (a feature it cannot provide is off here). A host twin
 * that SR_GPU_TWIN_COLOR turned on counts as given (ace_set_twin_running). */
const AceConfig *ace_requested(void);
const AceConfig *ace_effective(void);
/* The canonical line of the run ("off" for an authentic run). */
const char *ace_spec_text(void);
/* What the run is called on screen: "Enhanced", or "Ace" for a tier=ace line. */
const char *ace_tier_name(void);
/* A feature this host cannot provide: off for the run, named once on stderr and in the report.
 * The others stay on. */
void ace_feature_unavailable(AceFeature feature, const char *reason);
/* A feature that really ran: texture filtering once the GPU renderer made its pool for it, the
 * in-game UI's three once its pass is registered. At exit a feature that is on but was never
 * confirmed is reported unavailable (its renderer or presenter did not start), so the report
 * never claims what did not happen. */
void ace_confirm(AceFeature feature);
/* Matrix uses the culling hook left alone (its table was full): the report's cull_untracked. */
void ace_set_cull_untracked(unsigned long n);
/* peak=limit's totals for the report (driver.c reads them from the audio backend at exit): pushes
 * that carried a SAS sum past 16 bits, the frames of those that differ from the guest's saturated
 * copy, the output frames the limiter turned down, and its lowest gain (1 when it never acted). */
void ace_set_peak_stats(uint64_t pushes_wide, uint64_t frames_wide, uint64_t frames_limited, double min_gain);

/* ---- stick dead-zone remap (stick=<counts>, contract 11.3.11) ------------------------------- */

/* The host's stick dead zone in centred counts (0..127): the larger of the two axes'
 * deadzone_inner (0..32767 of a host axis) as (deadzone_inner * 255) / 65535. A negative value
 * reads as 0. */
int ace_stick_host_counts(int deadzone_inner_x, int deadzone_inner_y);
/* Remaps one left-stick sample (bytes 0..255, 128 at rest) so the host's live range starts past
 * a square dead zone of game_counts (1..126) that the title ignores. host_counts is the host's
 * own dead zone (ace_stick_host_counts): 127 or more, or below 0, reads as 0; any D in 0..126
 * is used as it is, at or above game_counts too. With h = byte - 128 (clamped to -127..127) and
 * m = max(|hx|, |hy|), the target magnitude is G + (m - D) * (127 - G) / (127 - D) past D and
 * m * (G - 1) / D up to it; both axes scale by that over m, rounded to nearest, signs kept, and
 * go back to bytes through the inverse of the title's centring (x >= 1: x + 128, else x + 127).
 * A sample at rest (m == 0), an axis at rest and a game_counts out of range are left as they
 * are; m == 127 maps to 127, so full throw is unchanged. With D < G no moved axis comes out at
 * rest. With D >= G an axis can: up to D (a mouse) the target is 0 when m * (G - 1) < D (G 10, D 100,
 * m 5), and past D the minor axis of a push can scale below half a count (G 10, D 30, (31, 1));
 * such an axis becomes byte 127, which the title reads as 0 (rest, 128, reads +1), inside its dead
 * zone either way. Pure: no state, no I/O. */
void ace_stick_remap(uint8_t *lx, uint8_t *ly, int game_counts, int host_counts);
/* The run's remap of one live host sample: does nothing unless Ace is on with stick=<counts>.
 * Counts the samples off rest it passed through the curve (the report's samples_remapped),
 * confirms the feature at the first one, and keeps host_counts for the report. */
void ace_stick_sample(uint8_t *lx, uint8_t *ly, int host_counts);

/* A vblank_wait_skip site of the title's Ace load part (contract 9.8 and 11.3.10), asked by
 * nk_ace_load_skip_wait() (recomp.h, src/rt/hle_display.c) once that call would block: nonzero
 * only while load=fast is in effect, and then the site's call to sceDisplayWaitVblankStart is
 * skipped. Each nonzero return is one skipped wait, counted
 * for the report (load_waits_skipped), and confirms the part. A run that asked for load=fast and
 * never reached a site reports it unavailable at exit: the title's build has no load part, or
 * none of its declared waits ran. site and pc name the declared site (its id and the jal's
 * address) in the guard's line below.
 *
 * The pacing guard (contract 11.3.10): a load wait is skipped in a loading screen, not in every
 * logic update, so the run counts consecutive logic updates (nk_ace_load_update(), which
 * generated code calls where each one begins) in each of which at least one wait was skipped.
 * When that run reaches ACE_LOAD_PACING_UPDATES, the skipped wait paces updates, not a load: from
 * then on nothing is skipped for the rest of the run, so every declared wait runs its stock call,
 * and one "ENHANCEMENT_LOAD_PACING:" line names the site whose skip reached it. The report carries
 * the longest run seen (load_skip_max_run) and whether the guard fired (load_paced). 8 is four
 * times the longest run measured through one title's loads (2 updates, contract 11.3.10), and
 * provisional until a second title is measured. */
#define ACE_LOAD_PACING_UPDATES 8u
int nk_ace_load_fast(const char *site, uint32_t pc);
/* One logic update begins (the package's update_boundary): ends the previous update's part of
 * the guard's run. Does nothing unless load=fast is in effect. */
void nk_ace_load_update(void);
/* The waits nk_ace_load_fast() has skipped in this run. */
unsigned long long ace_load_waits_skipped(void);
/* The longest run of consecutive logic updates with a skipped wait, and whether it reached
 * ACE_LOAD_PACING_UPDATES (skipping then stopped for the rest of the run). */
unsigned ace_load_skip_max_run(void);
int ace_load_paced(void);
/* An anisotropic sampler the renderer could not make: that draw kept the plain one. */
void ace_note_af_failure(void);
/* The pause menu's live Speed readout switch (Ace on only; a readout this host cannot draw
 * stays off). */
void ace_set_readout(int on);
/* A host twin (full 32-bit colour) that is running, whatever turned it on, the launch line or
 * SR_GPU_TWIN_COLOR: the effective config and the report say so, and it counts as confirmed. A
 * twin already named unavailable stays off. */
void ace_set_twin_running(void);
/* The anisotropy the device gives, at most the requested level (0 = none). */
void ace_set_af_effective(int af, const char *reason);
/* The supersampling factor the renderer could give (ENHANCEMENT_CONTRACT.md 10.21): below the
 * request, the run records the factor and `reason` and says so in one "ACE_MODE: ssaa" line; 1 or
 * less marks supersampling unavailable with `reason`. Does nothing unless ssaa was asked for. */
void ace_set_ssaa_effective(int factor, const char *reason);
/* The active features in words, joined by " \xC2\xB7 " ("16x filtering \xC2\xB7 crisp keyboard"),
 * for the pause menu caption and the launch notice. Empty with Ace off. */
size_t ace_feature_words(char *out, size_t cap);

/* ---- observation: the 30 Hz detector -------------------------------------------------------- */

/* The game's logic rate as VBLANKs per new frame. RATE_K1..K4 are 60, 30, 20 and 15 Hz. */
typedef enum {
    ACE_RATE_NONE = 0,        /* no class is stable */
    ACE_RATE_K1 = 1,
    ACE_RATE_K2 = 2,
    ACE_RATE_K3 = 3,
    ACE_RATE_K4 = 4,
    ACE_RATE_STATIC = 5,      /* no new frame for ACE_DET_STATIC_VBLANKS */
    ACE_RATE_VARIABLE = 6,    /* frames come at no one rate */
    ACE_RATE_COUNT
} AceRate;

#define ACE_DET_WINDOW 32u          /* intervals the class is judged over */
#define ACE_DET_STATIC_VBLANKS 30u  /* no new frame for this long reads STATIC */
#define ACE_DET_ENTER_VBLANKS 60u   /* a class is entered after 1.0 s of agreement */
#define ACE_DET_LEAVE_VBLANKS 30u   /* and left after 0.5 s of disagreement */

typedef struct {
    uint8_t interval[ACE_DET_WINDOW]; /* VBLANKs between consecutive new frames, ring */
    uint32_t count;                   /* intervals held, at most ACE_DET_WINDOW */
    uint32_t head;                    /* next slot to write */
    uint32_t last_new_vcount;
    int have_last;
    AceRate candidate;                /* the window's class, while it differs from stable */
    uint32_t candidate_since;
    AceRate stable;
    int disagreeing;
    uint32_t disagree_since;
    AceRate last_rate;                /* the last stable K class, kept through STATIC */
    int ever_stable;                  /* a K class has been stable once */
} AceDetector;

void ace_det_reset(AceDetector *d);
/* The class of a full window, or NONE before it is full. Class k (1 to 4) when the intervals
 * sum to between 32k - 2 and 32k + 4 and at least 28 of them are within one VBLANK of k; that
 * holds for "28 of 32 equal k, the rest k + 1" and for a 60 Hz game whose flips sometimes
 * share a slot (0, then 2). Otherwise VARIABLE. */
AceRate ace_det_window_class(const AceDetector *d);
/* One flip at `vcount`; new_frame is nonzero when the flipped target changed since its own
 * previous flip. Returns 1 when the stable class changed. */
int ace_det_flip(AceDetector *d, uint32_t vcount, int new_frame);
const char *ace_rate_name(AceRate rate);   /* "60 Hz", "30 Hz", ..., "static", "variable" */

/* ---- observation: the speed readout --------------------------------------------------------- */

/* One one-second window of observations. */
typedef struct {
    uint64_t wall_ns;          /* the window's length */
    uint32_t new_frames;       /* F's numerator */
    uint64_t idle_ns;          /* host time in the scheduler's two waits */
    uint32_t skipped_slots;    /* VBLANK deadlines vblank_pace had to skip */
    AceRate stable;            /* the detector's class at the window's end */
    int ever_stable;
    AceRate last_rate;         /* the last stable K class */
    int smooth;                /* Smooth motion is on for the run */
    uint32_t betweens;         /* in-betweens that reached the screen in the window */
    uint32_t shown;            /* distinct pictures that reached the screen: those, the held
                                * frames and the new frames shown as they are */
    uint32_t motion_milli;     /* the motion weights of the in-betweens measured in the window, in
                                * thousandths (ace_motion_weight_milli) */
} AceWindow;

typedef struct {
    int in_stretch;            /* inside a PC-limited stretch */
    int stretch_k;             /* the K class that was stable when it began (0: none yet) */
} AceReadoutState;

typedef struct {
    double fps;                /* F: new frames per second */
    double idle_share;         /* I: host idle time over wall time */
    int pc_limited;
    int have_target;           /* a percentage can be shown */
    double target_fps;         /* T = 59.94 / k */
    int speed_pct;             /* F / T while PC limited, else 100 */
    int smooth_on;             /* Smooth motion is on: the text says whether it added frames */
    double shown_fps;          /* with it on, pictures that reached the screen per second */
    double motion_fps;         /* with it on, the real pictures per second plus each in-between's
                                * motion weight: what "SMOOTH n" prints */
    char text[64];             /* "100%  30 FPS", "\xE2\x96\xB2 87%  26 FPS  PC LIMITED", "\xE2\x80\x94 27 FPS" */
} AceReadoutView;

void ace_readout_reset(AceReadoutState *s);
/* Judges one window; PC limited when a slot was skipped, or when the host idled under 2% while
 * frames came slower than the class allows. The target keeps the class stable when the stretch
 * began, so a steady slowdown does not redefine it. With Smooth motion on, the text ends with
 * "SMOOTH n" or, in a window it showed no in-between in (the game was not at a steady 30 FPS),
 * "SMOOTH OFF": it is never off without saying so. n counts what moved, not what was presented:
 * the real pictures that reached the screen in a second, plus each in-between by its measured
 * motion weight. An in-between that repeats the old frame adds nothing, so a scene the
 * in-betweens cannot follow reads SMOOTH 30 however many presents there were. */
void ace_readout_judge(AceReadoutState *s, const AceWindow *w, AceReadoutView *out);

/* ---- observation feed (Ace on only; callers check g_ace_on) ---------------------------------- */

/* A draw or block transfer was recorded for the target at VRAM offset `fb`. Counted when it is
 * recorded, before the target's batches are submitted, so a flip that latches them sees it. */
void ace_note_target_draw(uint32_t fb);
/* A CPU write (a store, a movie frame, a software-raster result) reached VRAM offsets
 * [off, off + bytes): every flipped buffer it overlaps has changed. */
void ace_note_vram_write(uint32_t off, uint32_t bytes);
/* Host time the scheduler spent waiting (vblank_pace, sleep_until_us), and slots it skipped. */
void ace_note_host_wait(uint64_t ns);
void ace_note_skipped_slots(uint32_t slots);
/* Fast forward started (1) or stopped (0) (sched.c; recomp.h, sched_fast_forward_on). The
 * readout's window is a second of wall time, so time at the fast-forward speed is never judged:
 * the readout hides, skipped slots are not counted, no window counts towards the report, and the
 * first full second at normal speed brings the readout back. */
void ace_note_fast_forward(int on);
/* The display flipped to the `bytes`-long buffer at VRAM offset `fb` at `vcount`
 * (display_present_active, before the presenter's own slot rule). Feeds the detector and,
 * once a second of `now_ns`, the readout. Returns 1 when the flip shows a new frame: the
 * buffer was drawn or written since its own previous flip, or it is not the buffer the
 * previous flip named. */
int ace_flip(uint32_t fb, uint32_t bytes, uint32_t vcount, uint64_t now_ns);
/* The readout as last judged; 0 before the first window. */
int ace_readout_view(AceReadoutView *out);
AceRate ace_rate_stable(void);
/* Observation counters (presenter cell A3: all zero with Ace off). */
typedef struct {
    uint64_t draws, flips, new_frames, host_wait_ns, skipped_slots, windows;
} AceCounters;
void ace_counters(AceCounters *out);

/* ---- smooth motion: which frame each present shows ------------------------------------------- */

/* A title that flips a new frame every second VBLANK leaves every other VBLANK without a present.
 * Smooth motion fills it: the flip of frame B shows the in-between of A and B, and the VBLANK
 * after it shows B, so B reaches the screen one VBLANK late and nothing is ever guessed ahead of
 * the guest. Host side only: these presents read private copies (gpu_sdl3vk/ace_smooth.h), and
 * the flip's own present still gives the capture and guest VRAM the real frame.
 *
 * Only the detector's stable 30 Hz class interpolates. Anything else shows each frame as it
 * comes (and keeps a copy, so the first frame of a 30 Hz stretch has a predecessor). */
typedef enum {
    ACE_SHOW_REAL = 0,     /* no new frame, or smooth off: this present is today's */
    ACE_SHOW_KEEP,         /* a new frame outside 30 Hz: show it and keep a copy */
    ACE_SHOW_BETWEEN,      /* a new 30 Hz frame: show the in-between, hold the frame */
    ACE_SHOW_OWED          /* a new frame while the one before it is owed: show that one, hold
                            * this one */
} AceShow;

typedef struct {
    AceShow show;          /* the flip's verdict, until its present takes it */
    int held;              /* a real frame the pass holds has not reached the screen */
    int owed;              /* its present came due and the display could not take it */
    uint32_t held_after;   /* the VCOUNT of the present that holds it */
    uint64_t flip_ns;      /* when the latest flip arrived */
    uint64_t held_ns;      /* when the in-between of the held frame was presented */
    uint64_t owed_ns;      /* the same for an owed frame the next flip's present shows */
    int owed_shown;        /* that present is on its way: its HELD note is the owed frame's */
    int reflip;            /* this flip shows the held frame again: it is the held present */
    int owed_next;         /* this OWED verdict is a re-flip's: the frame it holds is owed too */
    uint64_t frame_seq;    /* new frames flipped with smooth motion on */
    uint64_t taken_seq;    /* the newest of them whose verdict a present took */
    uint64_t owed_seq;     /* taken_seq before an OWED verdict was taken, for its present's failure */
    uint64_t unpaired;     /* in-betweens refused: the frame before never reached a present */
    uint64_t superseded;   /* held frames never shown: replaced by a flip before their VBLANK's
                            * present, at fast forward, or given up when a present failed and
                            * none can be tried again */
    uint64_t busy;         /* held presents the display could not take: busy, or failed and tried
                            * again by the present that takes over */
} AcePace;

void ace_pace_reset(AcePace *p);
/* A flip. The verdict stands until ace_pace_take. A flip of the unchanged frame (a title that
 * flips each frame twice) shows the held frame, so that present is the held one. A new frame
 * that arrives before the held frame's present supersedes it: smooth motion's one VBLANK of
 * delay does not fit a frame shown for one VBLANK. A flip that arrives while the held frame is
 * owed (its present came due and the display could not take it) never replaces it: the
 * verdict is OWED, the present shows the owed frame in place of an in-between, and the flip's
 * own frame is held. That holds for a new frame, and for a re-flip whose frame is newer than
 * the held one (the flips in between found the display busy, so no present took it). The
 * re-flip's present was that frame's own, so the frame it holds is owed in turn and the next
 * present shows it. So every frame whose present the display took is shown, in order, and a
 * display that takes only every second present gets the real frames. */
void ace_pace_flip(AcePace *p, int new_frame, AceRate rate, int smooth, uint64_t now_ns);
/* The flip's present: returns the verdict and clears it. An in-between is made only of two
 * consecutive frames: when the frame before this one never reached a present (the presenter's
 * slot rule dropped its flip, or the present failed), the pass holds an older frame, and an
 * in-between of the two would put whatever moved between them in the wrong place, or lose it.
 * Such a verdict becomes KEEP (the real frame is shown and kept) and is counted in `unpaired`.
 * An OWED verdict shows the held frame, which is always the pass's newest before this record,
 * however many flips the display missed; one left over after a present showed the held frame
 * becomes KEEP. */
AceShow ace_pace_take(AcePace *p);
/* The present that showed the owed frame failed after its submission, and the renderer can
 * present again: nothing reached the screen, and the pass has dropped the frame it recorded
 * (ace_smooth_unrecord). As when the display is busy, the frame stays owed and the OWED verdict
 * stands, so the present that takes over (gui_present's CPU present of the same flip) shows it;
 * failing that, the next VBLANK or the next flip. Counted in `busy`. */
void ace_pace_owed_failed(AcePace *p);
/* The in-between went to the screen at now_ns (the presenter's clock): the frame is held for the
 * VBLANK after `vcount`. Without smooth motion the frame would have been shown then, so the
 * held present's time less this one is the delay smooth motion adds. */
void ace_pace_shown_between(AcePace *p, uint32_t vcount, uint64_t now_ns);
/* The owed frame went to the screen with the flip of the frame after it, at now_ns: that frame
 * is now the held one, for the VBLANK after `vcount`. */
void ace_pace_shown_owed(AcePace *p, uint32_t vcount, uint64_t now_ns);
/* One VBLANK. Returns 1 when the held frame is due. It stays held until a present shows it
 * (ace_pace_held_shown) or gives it up (ace_pace_held_missed). */
int ace_pace_vblank(AcePace *p, uint32_t vcount);
/* A present showed a held frame. Returns when that frame's in-between was presented. */
uint64_t ace_pace_held_shown(AcePace *p);
/* The held frame's present was due and did not show it. `retry`: the display was too busy to
 * take a frame, so the frame is owed and tried again at the next VBLANK, or shown by the next
 * flip. Otherwise the present failed or the window closed, and the frame is given up and
 * counted: a failure that repeats (a minimised window, say) must not leave an old frame waiting
 * to be shown after the newer ones. */
void ace_pace_held_missed(AcePace *p, int retry);

typedef enum {
    ACE_PRESENT_REAL = 0,  /* the flip's own frame */
    ACE_PRESENT_BETWEEN,   /* an in-between */
    ACE_PRESENT_HELD,      /* the held frame, one VBLANK after its flip */
    ACE_PRESENT_KINDS
} AcePresentKind;

/* Present-to-present time in 1 ms bins, the last bin open-ended. */
#define ACE_PACE_BINS 50u

typedef struct {
    uint64_t kind[ACE_PRESENT_KINDS];
    uint64_t interval[ACE_PACE_BINS];
    uint64_t interval_count;
    uint64_t interval_sum_ns;
    uint64_t hold_count;
    uint64_t hold_sum_ns;         /* flip to held present: the delay smooth motion adds */
    uint64_t hold_max_ns;
    /* The pace machine's counts (AcePace), filled in by ace_pace_stats; ace_pace_stats_add
     * leaves them alone. */
    uint64_t superseded;          /* held frames that never reached the screen */
    uint64_t busy;                /* held presents the display could not take */
    uint64_t last_ns;
} AcePaceStats;

/* The kind of the flip's present when it shows no in-between: REAL, or HELD for a re-flip. */
AcePresentKind ace_pace_flip_kind(const AcePace *p);

/* The run's verdict for the flip ace_flip just saw. */
AceShow ace_smooth_take(void);
/* The present of an in-between the show pass made reached the screen: the frame is held for the
 * VBLANK after. It counts nothing; ace_smooth_note_present counts what the swapchain accepted. */
void ace_smooth_shown_between(uint64_t now_ns);
/* The present that showed the owed frame in place of an in-between reached the screen, and the
 * show pass kept the new frame: that one is held for the VBLANK after. When that present fails
 * after its submission, the owed frame stays owed for the present that takes over
 * (ace_smooth_owed_failed); when it failed before, or the renderer can present nothing any more,
 * it is given up (ace_smooth_held_missed with retry 0). */
void ace_smooth_shown_owed(uint64_t now_ns);
/* ace_pace_owed_failed for the run. */
void ace_smooth_owed_failed(void);
/* One VBLANK: 1 when the held frame should be presented now. Off, it reads nothing. */
int ace_smooth_vblank(uint32_t vcount);
/* The held frame's present did not show it (ace_pace_held_missed). */
void ace_smooth_held_missed(int retry);
/* A flip's own present found the display busy. When that flip re-showed the held frame, the
 * frame is owed. */
void ace_smooth_flip_busy(void);
/* A present reached the swapchain (the presenter accepted it). A flip's present that showed no
 * in-between is passed as REAL and counted as HELD when it re-showed the held frame. This is also
 * what the readout's "SMOOTH n" counts: an in-between, a held frame, and a flip's own frame when
 * it was not shown before; a present the presenter dropped is not noted and not counted. */
void ace_smooth_note_present(AcePresentKind kind, uint64_t now_ns);
void ace_pace_stats(AcePaceStats *out);
void ace_pace_stats_add(AcePaceStats *s, AcePresentKind kind, uint64_t now_ns);

/* ---- smooth motion: what an in-between moved ------------------------------------------------- */

/* The presenter measures every in-between it makes against its two real frames
 * (gpu_sdl3vk/ace_smooth.h, AceSmoothMotion): over the pixels that differ between the two frames,
 * `sum_old` and `sum_new` are the in-between's summed distance to the old and to the new frame.
 * Its phase is sum_old / (sum_old + sum_new): 0 for a repeat of the old frame, one half when the
 * changing content sits half-way, 1 for the new frame. The weight is how much of a picture of
 * its own that makes it, in thousandths: 2 * min(sum_old, sum_new) / (sum_old + sum_new), so 0
 * at either end and 1000 at one half, and 0 when nothing differs. A plain blend of the two
 * frames also weighs 1000: the weight says the content is between the frames, not that it is
 * sharp. */
uint32_t ace_motion_weight_milli(uint32_t sum_old, uint32_t sum_new);

typedef struct {
    uint64_t measured;      /* in-betweens measured */
    uint64_t moving;        /* sampled pixels that differ between the two real frames */
    uint64_t eq_old;        /* of those, shown as the old frame */
    uint64_t eq_new;        /* of the rest, shown as the new frame */
    uint64_t sum_old;       /* summed distance of the moving pixels to the old frame */
    uint64_t sum_new;       /* and to the new frame */
    uint64_t novel;         /* pixels the two frames agree on that the in-between shows otherwise */
    uint64_t weight_milli;  /* the sum of the in-betweens' weights */
    uint64_t unread;        /* in-betweens whose measurement could not be read */
} AceMotionStats;

/* One in-between's measurement, read once its commands have finished on the GPU. Feeds the
 * readout's window and the report's `smooth_motion`. */
void ace_smooth_note_motion(uint32_t moving, uint32_t eq_old, uint32_t eq_new, uint32_t sum_old,
                            uint32_t sum_new, uint32_t novel);
/* An in-between whose measurement was gone when it came due: counted, never guessed. */
void ace_smooth_note_motion_unread(void);
void ace_motion_stats(AceMotionStats *out);

/* When a measurement may be read. The show pass records an in-between and its measurement into
 * the present's command buffer, which goes to the GPU in one numbered submission. The presenter
 * counts the submissions it has seen finished, in submission order (gpu_sdl3vk/sdl3vk.h,
 * sdl3vk_present_completed), so a measurement may be read once that count has reached its
 * submission. Pure: the presenter feeds it (gui.c) and reads what is due. */
#define ACE_MOTION_PENDING 8u
typedef struct {
    uint64_t serial[ACE_MOTION_PENDING];
    uint64_t submission[ACE_MOTION_PENDING];
    uint32_t head, count;
    uint64_t dropped;       /* serials dropped because the queue was full */
} AceMotionQueue;

void ace_motion_queue_reset(AceMotionQueue *q);
/* In-between `serial` reached the screen, its commands in `submission`. Returns 0 when the queue
 * is full; the serial is then dropped and counted. */
int ace_motion_queue_made(AceMotionQueue *q, uint64_t serial, uint64_t submission);
/* The oldest serial whose submission has finished by `completed` (the newest submission seen
 * finished), removed from the queue; 0 when none is due. */
uint64_t ace_motion_queue_due(AceMotionQueue *q, uint64_t completed);

/* What smooth motion's in-betweens read besides the pixels (gpu_sdl3vk/ace_hud.c and ace_smooth.c,
 * contract section 10.6), counted so a title where the HUD rule or the depth gate never applies
 * shows in the report instead of looking like one where it works. */
typedef struct {
    /* The boxes of through-mode draws (ace_hud.c). */
    uint64_t boxes_kept;          /* kept as HUD */
    uint64_t boxes_merged;        /* the second triangle of a quad: the same box again */
    uint64_t boxes_large;         /* a quarter of the screen or more: a backdrop */
    uint64_t boxes_target;        /* samples a render target: a post effect */
    uint64_t boxes_outside;       /* nothing on screen (off the edge, empty or not a number) */
    uint64_t boxes_full;          /* past the 1024-box or two-screen limit: that frame has no mask */
    /* What each GPU present published. */
    uint64_t frames_mask;         /* with a mask */
    uint64_t frames_overflow;     /* without one: its list went past a limit */
    uint64_t frames_depth;        /* with depth */
    uint64_t frames_depth_unwritten; /* without: no draw wrote that depth image since it was last
                                      * handed on, so it holds an older frame's depth */
    uint64_t frames_depth_none;   /* without: the target has no depth image */
    uint64_t published_unshown;   /* published, but the show pass took another image (a CPU frame,
                                   * or a present that never reached it) */
    /* The pairs an in-between was made from (ace_smooth.c). */
    uint64_t pairs;
    uint64_t pairs_mask;          /* both frames had a mask */
    uint64_t pairs_depth;         /* both frames had depth */
    uint64_t depth_refused;       /* frames whose depth image was smaller than the frame */
} AceSmoothAuxStats;

/* The renderer's latest totals, for the report and the exit line. */
void ace_smooth_aux_report(const AceSmoothAuxStats *s);

/* ---- the per-run report -------------------------------------------------------------------- */

/* The worst case, every block present and every number at its widest, is about 5.7 KB with
 * stick, load, depth, twin, ssaa, peak and glyphs in the spec; ace_selftest's cell_report_worst
 * checks that it fits. */
#define ACE_REPORT_MAX 6144u
/* The report's JSON for the run so far; 0 when it would not fit. */
size_t ace_report_json(char *out, size_t cap);
/* Settles the features that never ran (ace_confirm), then writes the report (SR_ACE_REPORT,
 * through a temporary file that atomically replaces the old report) and the exit stamp. An
 * empty SR_ACE_REPORT writes no file. Ace off: does nothing. Safe to call more than once. */
void ace_finish(void);
/* A callback ace_finish runs first, so a count the runtime keeps (the culling hook's untracked
 * matrices) is read after the last frame on every exit path that writes the report, the exit-at-VBLANK one
 * included.
 * It may call ace_confirm, ace_feature_unavailable and ace_set_cull_untracked; it must be safe to
 * run more than once. NULL clears it. */
void ace_set_finish_hook(void (*hook)(void));

/* ---- fast forward's keys (recomp.h, sched_fast_forward_on) ---------------------------------- */
/* The game window sets these: a hold (Tab while it is held) and a toggle (F9). They only ask;
 * the scheduler decides at each VBLANK whether this run may fast forward. */
void ace_ff_hold(int on);
void ace_ff_toggle(void);
int ace_ff_requested(void);   /* 1 while the hold or the toggle is on */

#endif /* SR_ACE_H */
