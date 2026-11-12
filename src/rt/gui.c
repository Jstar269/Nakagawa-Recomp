// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

/* The game's window: presents the PSP framebuffer each frame and feeds keyboard and gamepad
 * input to sceCtrl, so the recompiled game runs like a normal app (watch the intro, press START,
 * play). gui_init() picks the presenter, gui_present() paces and shows a frame, and the
 * gui_pause_* functions run the pause overlay.
 *
 * The default presenter is SDL3 + Vulkan (src/rt/gpu_sdl3vk), which owns the window and ALL input
 * devices: the SDL3 gamepad subsystem plus an SDL keyboard fallback. gui.c
 * forwards that state to the runtime. When SR_VIDEO=gdi selects the legacy Win32/GDI window
 * (a top-level window + a 32-bit DIB the PSP framebuffer is converted into each frame), input
 * falls back to a keyboard-only GetAsyncKeyState mapping; controllers on that debug path are not
 * supported (no XInput/DirectInput dependency).
 * SR_VIDEO=offscreen is an explicit host-memory sink for headless bring-up: it converts validated
 * guest frames without initializing SDL video, creating a window, or claiming a presentable
 * Vulkan surface, and it provides no live input device.
 *
 * The SDL3/Vulkan presenter is host-neutral and is the only presenter on a host without
 * Win32/GDI: everything GDI-shaped (the window class, the DIB, GetAsyncKeyState, the message
 * pump) is behind _WIN32, and a host without it gets the SDL3 path or, if that cannot
 * initialise, a reported refusal (no window, s_on == 0) rather than a fabricated one. */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "../core/nk_shared_helpers.h"
#include "gpu_sdl3vk/sdl3vk.h"
#include "fbcap.h"
#include "videorec.h"         /* video recording: the CPU presenters hand over their frame */
#include "osk_overlay.h"        /* the in-window keyboard: keys in, the frame drawn over */
#include "osk_overlay_paint.h"
#include "ace.h"             /* Ace Mode: crisp keyboard; refused by the acceptance checkpoint */
#include "ge_shared.h"       /* the software renderer's twin rows (#2050) and the VRAM viewer's
                                decoders (#314) */
#ifdef SR_SDL3VK
#include "gpu_sdl3vk/ge_gpu.h"
#include "gpu_sdl3vk/ace_smooth.h" /* Ace Mode's in-between frames */
#include "gpu_sdl3vk/ace_hud.h"    /* ... and the HUD mask and depth they read */
#include "gpu_sdl3vk/fsr1_pass.h"  /* the presentation upscaler (SR_PRESENT_UPSCALE) */
#include "gpu_sdl3vk/anime4k_pass.h" /* the movie upscaler (SR_MOVIE_UPSCALE) */
#endif
#if defined(SR_SDL3VK) && !defined(SR_GUI_PRESENT_SELFTEST)
#include "nk_ui_theme.h"         /* the pause overlay's scrim token */
#include "overlay/overlay_host.h" /* the pause overlay's pages (C9c) */
#include "gpu_sdl3vk/ace_ui.h"   /* Ace Mode's in-game UI: notice, readout, crisp keyboard */
#include "nk_platform.h"         /* the system's Reduce motion, for the Ace UI */
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>
#endif
#ifdef SR_GUI_PRESENT_SELFTEST
#include "overlay/nk_overlay.h"  /* the pause display rows' actions and pacing choices */
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <stdint.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PSP_W 480
#define PSP_H 272
#define PSP_SCANOUT_RATE_NUMERATOR 60000ull
#define PSP_SCANOUT_RATE_DENOMINATOR 1001ull
#define PSP_SCANOUT_PERIOD_NS \
    ((PSP_SCANOUT_RATE_DENOMINATOR * 1000000000ull + PSP_SCANOUT_RATE_NUMERATOR / 2u) / \
     PSP_SCANOUT_RATE_NUMERATOR)

enum {
    PRESENT_CAP_ENV_UNRESOLVED = -1,
    PRESENT_CAP_SCANOUT = -2
};

/* gui-present-selftest compiles this file alone. It exercises the host present pacing below
 * and, with the SDL3 presenter, the input and quit hand-off after Ace Mode's held present
 * (gui_ace_vblank): the events that present reads must reach the game's input state before
 * its controller sample, and a closed window must request quit. That hand-off and the input
 * state it fills (SR_GUI_INPUT_PATH) run against the sdl3vk and runtime stubs in the selftest
 * block at the end of this file; each stub the test does not expect to be called fails it. So do
 * smooth motion's show pass and what gui_present does for Ace Mode after its presents, against a
 * stubbed renderer.
 * The presenters themselves need guest memory, the scheduler, perf telemetry and the Vulkan
 * backend, so they stay out of that binary: a pacing regression must not depend on a stubbed
 * presenter. */
#if !defined(SR_GUI_PRESENT_SELFTEST) || defined(SR_SDL3VK)
#define SR_GUI_INPUT_PATH 1
#endif
#ifdef SR_GUI_INPUT_PATH
static int      s_on = 0;
#ifdef SR_SDL3VK
static int      s_sdl3 = 0;            /* SDL3+Vulkan presenter active (src/rt/gpu_sdl3vk) */
static AceBetween *s_ace_smooth;       /* Ace Mode's in-between frames, when smooth motion is on */
static Fsr1Pass *s_fsr1;               /* the presentation upscaler, when SR_PRESENT_UPSCALE=fsr1 */
static Fsr1Pass *s_fsr1_rec;           /* its own instance for the recording (#2138), made at the
                                          first recorded present */
static int s_fsr1_rec_failed;          /* that instance could not be made: said once, not retried */
/* The movie upscaler (SR_MOVIE_UPSCALE) for CPU-presented frames: Anime4K's CNN stages, then FSR 1 to
 * the exact size. The window and the recording each own a set, as for the presentation upscaler. */
typedef struct {
    Anime4kPass *anime4k;
    Fsr1Pass *fsr1;
    int failed;                        /* the set could not be made: said once, not retried */
    unsigned long long presents;       /* presents it made, said at shutdown */
    unsigned long long anime4k_presents;  /* of those, the ones Anime4K's stages made */
} MovieUpscale;
static int s_movie_mode;               /* MOVIE_UPSCALE_* */
static MovieUpscale s_movie, s_movie_rec;
#endif
static int      s_offscreen = 0;        /* explicit host-memory presenter for headless bring-up */
static uint32_t s_buttons = 0;
static uint8_t  s_lx = 128, s_ly = 128;   /* live left-stick (0..255, 128=centre), latched each present */
static int      s_stick_dead_zone = 0;     /* the input profile's stick dead zone in centred counts,
                                              latched with the stick; 0 without an SDL3 presenter */
static int      s_pad_present = 0;         /* a controller is currently connected */
#endif /* SR_GUI_INPUT_PATH */
#ifndef SR_GUI_PRESENT_SELFTEST
#if defined(SR_SDL3VK) || defined(_WIN32)
static bool     s_foreground_request_issued;
#endif
static uint64_t s_offscreen_frames;
static int      s_headless_accept_parsed;
static int      s_headless_accept_requested;
static int      s_headless_accept_invalid;
static int      s_headless_accept_ace;   /* refused: an Ace Mode run (ace.h) */
static int      s_headless_accept_ready;
static int      s_headless_accept_complete;
static uint32_t s_headless_accept_target;
static uint32_t s_headless_accept_presented;
#ifdef _WIN32
static HWND     s_hwnd;
static BITMAPINFO s_bmi;
#endif
static uint32_t *s_px;                 /* PSP_W*PSP_H BGRA for every presenter, including offscreen */
static uint64_t s_last_ns;             /* fallback frame-pacing deadline */
/* The framebuffer the guest last had displayed: a pause freezes its current contents. */
static struct { uint32_t addr; int fmt; uint32_t stride; int valid; } s_shown_fb;
#endif /* !SR_GUI_PRESENT_SELFTEST */
static uint64_t s_present_next_ns;      /* host output cap; never delays guest execution */
static int      s_present_cap = PRESENT_CAP_ENV_UNRESOLVED;

#ifdef SR_GUI_INPUT_PATH
/* Keep boot milestones observable to the parent player without depending on
 * whether the platform process backend inherits stderr. The file is opt-in and
 * is otherwise not opened by the runtime. */
static void sr_gui_boot_event(const char *format, ...) {
    uint64_t timestamp_ns = sr_perf_now_ns();
    va_list args;
    va_start(args, format);
    nk_shared_boot_event_vwrite(stderr, timestamp_ns, format, args);
    va_end(args);
}
#endif /* SR_GUI_INPUT_PATH */

#ifndef SR_GUI_PRESENT_SELFTEST
static void gui_headless_acceptance_parse(void) {
    if (s_headless_accept_parsed) return;
    s_headless_accept_parsed = 1;
    const char *value = getenv("SR_HEADLESS_ACCEPT_PRESENTED_FRAMES");
    if (!value || !value[0]) return;
    s_headless_accept_requested = 1;
    /* The checkpoint is evidence, and an Ace Mode run (ace.h) is enhanced: refused, named. */
    if (g_ace_on) {
        fprintf(stderr, "ACE_MODE: the acceptance checkpoint needs an authentic run; refused\n");
        s_headless_accept_invalid = 1;
        s_headless_accept_ace = 1;
        return;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || end == value || !end || *end || parsed == 0u ||
        parsed > UINT32_MAX) {
        s_headless_accept_invalid = 1;
        return;
    }
    s_headless_accept_target = (uint32_t)parsed;
}

int gui_headless_acceptance_requested(void) {
    gui_headless_acceptance_parse();
    return s_headless_accept_requested;
}

int gui_headless_acceptance_ready(void) {
    gui_headless_acceptance_parse();
    return s_headless_accept_requested && !s_headless_accept_invalid &&
        s_headless_accept_ready;
}

int gui_headless_acceptance_complete(void) {
    return s_headless_accept_complete;
}

uint32_t gui_headless_acceptance_presented_frames(void) {
    return s_headless_accept_presented;
}

static void gui_headless_acceptance_record_present(int accepted) {
    if (!accepted || !s_headless_accept_requested ||
        !s_headless_accept_ready || s_offscreen ||
        s_headless_accept_complete) return;
    s_headless_accept_presented++;
    if (s_headless_accept_presented >= s_headless_accept_target) {
        s_headless_accept_complete = 1;
    }
}
#endif /* !SR_GUI_PRESENT_SELFTEST */

/* A guest may call sceDisplaySetFrameBuf several times during one scanout interval;
 * presenting more often than the display refresh wastes GPU/WSI work. The default
 * period uses the same 60000/1001 Hz scanout rate as the scheduler. */
static int present_cap_from_value(const char *value) {
    if (!value || !value[0] || strcmp(value, "native") == 0) {
        return PRESENT_CAP_SCANOUT;
    }
    char *end = NULL;
    long cap = strtol(value, &end, 10);
    if (end == value || *end != '\0' || cap < 0) return PRESENT_CAP_SCANOUT;
    if (cap > 240) return 240;
    return (int)cap;
}

static uint64_t present_period_for_cap(int cap) {
    if (cap == PRESENT_CAP_SCANOUT) return PSP_SCANOUT_PERIOD_NS;
    return cap ? 1000000000ull / (uint64_t)cap : 0;
}

/* The output cap in force: SR_FPS_CAP until something changes it. */
static int present_cap(void) {
    if (s_present_cap == PRESENT_CAP_ENV_UNRESOLVED)
        s_present_cap = present_cap_from_value(getenv("SR_FPS_CAP"));
    return s_present_cap;
}

static uint64_t present_period_ns(void) {
    return present_period_for_cap(present_cap());
}

/* Changes the output cap for the rest of the run (the pause overlay's Frame pacing row, #2002).
 * The slot grid starts again, so the first present after the change is due at once under the
 * new period instead of waiting out a slot the old one set. The guest never waits on either. */
#if defined(SR_SDL3VK) || defined(SR_GUI_PRESENT_SELFTEST)
static void present_set_cap(int cap) {
    s_present_cap = cap == PRESENT_CAP_SCANOUT || (cap >= 0 && cap <= 240)
        ? cap : PRESENT_CAP_SCANOUT;
    s_present_next_ns = 0;
}

/* The pause overlay's display rows (#2002), kept apart from the window and the page so the
 * present selftest checks them. Each row uses the readout's convention: 0 no row, 1 off (or
 * the first choice), 2 on. Fullscreen has no row where the window cannot change mode (-1);
 * frame pacing has none for a developer's numeric SR_FPS_CAP, which is neither choice. */
static int pause_fullscreen_row(int window_fullscreen) {
    return window_fullscreen < 0 ? 0 : window_fullscreen ? 2 : 1;
}

static int pause_pacing_row(int cap) {
    return cap == PRESENT_CAP_SCANOUT ? 1 + NK_OVERLAY_PACING_PSP
         : cap == 0                   ? 1 + NK_OVERLAY_PACING_UNCAPPED
                                      : 0;
}

/* The cap a Frame pacing choice sets: a value the row cannot send is PSP timing, never
 * uncapped. */
static int pause_pacing_cap(int pacing) {
    return pacing == NK_OVERLAY_PACING_UNCAPPED ? 0 : PRESENT_CAP_SCANOUT;
}

/* Applies the Fullscreen or Frame pacing row's choice and returns 1, or returns 0 for any
 * other action. The window call is passed in, so the selftest runs this without a window. */
static int pause_display_act(NkOverlayAction action, int (*set_fullscreen)(int on)) {
    switch (action.kind) {
    case NK_OVERLAY_ACT_SET_FULLSCREEN:
        (void)set_fullscreen(action.value != 0);
        return 1;
    case NK_OVERLAY_ACT_SET_PACING:
        present_set_cap(pause_pacing_cap(action.value));
        fprintf(stderr, "SR_PAUSE: frame pacing %s\n",
                present_cap() == 0 ? "no extra cap" : "PSP timing");
        return 1;
    default:
        return 0;
    }
}
#endif

/* Output slots sit on a host-time grid one period apart, its phase set by the first present.
 * A submission takes the slot nearest to it, so one that arrives up to half a period before
 * the next slot is still presented. Guest VBLANKs reach the host with scheduling jitter: with
 * a floor rule (present only once the slot has started), every flip that came a little ahead
 * of the grid's phase was dropped although it was the only flip in its scanout. The grid
 * still advances one period per present, so the long-run rate never exceeds the cap, and a
 * second submission inside the same slot is dropped. */
static int present_slot_due_at(uint64_t now) {
    uint64_t period = present_period_ns();
    if (!period) return 1;

    uint64_t early = period / 2u;
    if (s_present_next_ns && now + early < s_present_next_ns) return 0;
    if (!s_present_next_ns || now > s_present_next_ns + period * 4u) {
        s_present_next_ns = now + period;
    } else {
        do { s_present_next_ns += period; } while (s_present_next_ns <= now);
    }
    return 1;
}

static int present_slot_due(void) {
    return present_slot_due_at(SDL_GetTicksNS());
}

#ifdef SR_SDL3VK
/* Whether a game-frame present may return DISPLAY_BUSY instead of waiting for the display
 * (sdl3vk_set_present_nowait). Only while the scheduler paces the guest to real time: a
 * deterministic-clock or turbo run has no real-time clock to protect and keeps every frame. And
 * never for a frame an armed file or evidence capture is waiting for (SR_FBDUMP, SR_FBSNAP, the
 * capture tools): that frame waits and is served, as before. The picture probe is not such a
 * capture; a probe on a frame the display could not take resolves as nothing attempted. */
static int gui_frame_may_skip_busy_display(int paced, int capture_armed, int capture_is_probe) {
    return paced && !(capture_armed && !capture_is_probe);
}

/* What gui_present does with gegpu_present's result. Only TRY_CPU reaches the CPU path, which
 * converts guest VRAM: a frame the display was too busy to take is not shown at all, never
 * redrawn from guest memory the GPU frame may not have reached. */
enum { GUI_GPU_SHOWN, GUI_GPU_CLOSED, GUI_GPU_NOT_SHOWN, GUI_GPU_TRY_CPU };
static int gui_gpu_result_action(int res) {
    switch (res) {
    case GEGPU_PRESENT_SHOWN:        return GUI_GPU_SHOWN;
    case GEGPU_PRESENT_WINDOW_CLOSED: return GUI_GPU_CLOSED;
    case GEGPU_PRESENT_DISPLAY_BUSY: return GUI_GPU_NOT_SHOWN;
    default:                         return GUI_GPU_TRY_CPU;   /* NOT_RESIDENT, TARGET_FAILED */
    }
}

/* The display could not take gui_present's frame: the GPU present found it busy (and the CPU
 * path did not run), or the CPU present did. `cpu_result` is GUI_CPU_NOT_RUN when there was none. */
enum { GUI_CPU_NOT_RUN = -2 };
static int gui_present_busy(int gpu_action, int cpu_result) {
    return gpu_action == GUI_GPU_NOT_SHOWN || cpu_result == SDL3VK_PRESENT_DISPLAY_BUSY;
}

/* ---- Ace Mode's smooth motion: the show pass and what became of its present (ace.h,
 * gpu_sdl3vk/ace_smooth.h) ------------------------------------------------------------------------
 * gui-present-selftest compiles these with the renderer's calls stubbed, so its runs drive this
 * code rather than a copy of it. The show pass decides what a present shows; what that changes
 * for the pace is settled only once the present has reached the screen (gui_ace_show_done). */
static AcePresentKind s_ace_present_kind;
static AceShow s_ace_show_pending;     /* what the show pass showed, until its present ends */
static uint64_t s_ace_between_serial;  /* the in-between it showed, for its measurement */
static AceMotionQueue s_ace_motion_queue;
/* A measurement is read at the first show pass after its submission was seen finished. That pass
 * waited for its own frame slot, so fewer than SDL3VK_PRESENT_FRAMES later submissions can still
 * be in flight then, and fewer in-betweens than the ring keeps were measured over its record. */
_Static_assert(SDL3VK_PRESENT_FRAMES < ACE_SMOOTH_MOTION_RING,
               "the measurement ring must outlast the presents in flight");

/* What the in-betweens moved: each one is measured in its own command buffer (ace_smooth.h), and
 * its numbers are read here once the presenter has seen that submission finished (ace.h,
 * AceMotionQueue). One that cannot be read then is counted, never guessed. */
static void gui_ace_collect_motion(void) {
    uint64_t serial;
    while ((serial = ace_motion_queue_due(&s_ace_motion_queue, sdl3vk_present_completed())) != 0u) {
        AceSmoothMotion m;
        if (ace_smooth_motion(s_ace_smooth, serial, &m))
            ace_smooth_note_motion(m.moving, m.eq_old, m.eq_new, m.sum_old, m.sum_new, m.novel);
        else
            ace_smooth_note_motion_unread();
    }
}

static void *gui_ace_show_pass(void *cmd, void *src, int *w, int *h) {
    VkCommandBuffer vk_cmd = (VkCommandBuffer)cmd;
    s_ace_present_kind = ACE_PRESENT_REAL;
    s_ace_show_pending = ACE_SHOW_REAL;
    gui_ace_collect_motion();
    if (!src) {
        VkImage held = ace_smooth_held(s_ace_smooth, vk_cmd);
        if (!held) return NULL;
        ace_smooth_size(s_ace_smooth, w, h);
        s_ace_present_kind = ACE_PRESENT_HELD;
        return (void *)held;
    }
    /* What the GPU GE published for this image (its HUD boxes and depth), taken on every real
     * present so it never outlives its frame; NULL for a CPU frame or another image. */
    const AceSmoothAux *aux = ace_hud_take((VkImage)src);
    AceShow show = ace_smooth_take();
    VkImage between = VK_NULL_HANDLE;
    if (show != ACE_SHOW_REAL)
        between = ace_smooth_record_aux(s_ace_smooth, vk_cmd, (VkImage)src, *w, *h,
                                        show == ACE_SHOW_BETWEEN, aux);
    /* The report's counts of boxes, frames and pairs with and without mask or depth. */
    AceSmoothAuxStats st;
    memset(&st, 0, sizeof(st));
    ace_hud_stats(&st);
    ace_smooth_aux_stats(s_ace_smooth, &st);
    ace_smooth_aux_report(&st);
    if (show == ACE_SHOW_OWED) {
        /* The frame before this one is owed: the display could not take its present. It is
         * shown now, in place of an in-between, and the frame just kept is held for the next
         * VBLANK (ace.h). A pass that no longer has it (the picture changed size) shows the new
         * frame as it is, and the owed one is counted as never shown. */
        VkImage owed = ace_smooth_previous(s_ace_smooth, vk_cmd);
        if (!owed) {
            ace_smooth_held_missed(0);
            return NULL;
        }
        ace_smooth_size(s_ace_smooth, w, h);
        s_ace_show_pending = ACE_SHOW_OWED;
        s_ace_present_kind = ACE_PRESENT_HELD;
        return (void *)owed;
    }
    if (!between) return NULL;
    s_ace_show_pending = ACE_SHOW_BETWEEN;
    s_ace_between_serial = ace_smooth_between_serial(s_ace_smooth);
    s_ace_present_kind = ACE_PRESENT_BETWEEN;
    return (void *)between;
}

/* The present the show pass recorded into ended (sdl3vk.h, Sdl3VkShowDone). Shown, the owed frame
 * or the in-between leaves the new frame held for the VBLANK after, and the in-between's
 * measurement waits for its submission. Failed (an out-of-date swapchain, or a GPU present the CPU
 * path then takes over), neither reached the screen. When the present failed after its
 * submission and the renderer can present again, the owed frame stays owed, as for a busy
 * display, and the pass drops the new frame it recorded, so the CPU present that follows shows
 * the owed frame and holds the new one again (ace.h, ace_pace_owed_failed). Failed before its
 * submission, the pass's commands never ran, so the frame is given up and counted. The new frame of a
 * failed in-between is not held, so the CPU present shows it as itself and no VBLANK shows it
 * again, and an in-between nobody saw is not measured. */
static void gui_ace_show_done(int result, uint64_t submission) {
    const AceShow shown = s_ace_show_pending;
    s_ace_show_pending = ACE_SHOW_REAL;
    if (shown == ACE_SHOW_OWED) {
        if (result == 1) {
            ace_smooth_shown_owed(SDL_GetTicksNS());
        } else if (submission && !sdl3vk_renderer_terminal()) {
            ace_smooth_unrecord(s_ace_smooth);
            ace_smooth_owed_failed();
        } else {
            ace_smooth_held_missed(0);
        }
    } else if (shown == ACE_SHOW_BETWEEN && result == 1) {
        ace_smooth_shown_between(SDL_GetTicksNS());
        if (!ace_motion_queue_made(&s_ace_motion_queue, s_ace_between_serial, submission))
            ace_smooth_note_motion_unread();
    }
}

/* The end of a gui_present, for Ace Mode. Every Ace run times its presents, so a run without
 * smooth motion is the baseline. A flip that re-shows the held frame is that frame's present:
 * when the display could not take it, the frame is owed (ace.h). */
static void gui_ace_presented(int accepted, int display_busy) {
    if (!g_ace_on) return;
    if (accepted) {
        ace_smooth_note_present(s_ace_smooth ? s_ace_present_kind : ACE_PRESENT_REAL, SDL_GetTicksNS());
        /* An in-between stays on screen until the held frame's VBLANK, so a recording keeps it
         * for its own VBLANK and writes the held frame at the next (videorec.h). */
        if (s_ace_smooth && s_ace_present_kind == ACE_PRESENT_BETWEEN) sr_videorec_frame_keep();
    }
    if (display_busy) ace_smooth_flip_busy();
}

/* ---- what the GPU GE renderer hands to the software renderer (#2151) -------------------------
 * While the GPU GE runs, a primitive in a state it cannot draw goes to ge.c's software rasterizer,
 * and when its target was on the GPU, with a synchronous readback (ge_gpu.c,
 * software_fallback_begin). gegpu_fallback_counts counts both by reason, always. After each
 * accepted present this reads them and steps nk_render_fallback_window_step, the rule in
 * nk_shared_helpers.h that decides when a reason costs enough for long enough to tell the
 * person: then it is reported once per run as BOOT_EVENT phase=render_fallback, and the player
 * names it on the game's page. Reading the counts changes nothing the GE draws.
 * gui-present-selftest drives this code with the counts stubbed. */
static NkRenderFallbackWindow s_render_window;
static int s_render_stats = -1;   /* SR_GPU_STATS: a line for each present that fell back; -1 unread */

/* After each gui_present: an accepted present steps the windows. With SR_GPU_STATS, a present
 * that handed anything to the software renderer also prints its counts, the figures the rule is
 * checked against (src/rt/gpu_sdl3vk/README.md); a present that did not prints nothing. */
static void gui_render_fallback_presented(int accepted) {
    if (!accepted) return;
    GeGpuFallbackCounts now;
    NkRenderFallbackDelta delta;
    gegpu_fallback_counts(&now);
    const uint16_t crossed = nk_render_fallback_window_step(&s_render_window, &now, &delta);
    if (s_render_stats < 0) {
        const char *st = getenv("SR_GPU_STATS");
        s_render_stats = st && st[0] && strcmp(st, "0") != 0;
    }
    if (s_render_stats) {
        char line[512];
        int used = snprintf(line, sizeof(line), "GEGPU_FALLBACK present gpu=%lu",
                            (unsigned long)delta.gpu_prims);
        int fell = 0;
        for (int r = 0; r < GEGPU_FALLBACK_REASONS && used > 0 && (size_t)used < sizeof(line); r++) {
            if (!delta.sw_prims[r] && !delta.readbacks[r]) continue;
            fell = 1;
            used += snprintf(line + used, sizeof(line) - (size_t)used, " %s=%llu/%llu",
                             gegpu_fallback_token(r), (unsigned long long)delta.sw_prims[r],
                             (unsigned long long)delta.readbacks[r]);
        }
        if (fell) fprintf(stderr, "%s\n", line);
    }
    for (int r = 0; r < GEGPU_FALLBACK_REASONS; r++) {
        if (!(crossed & (1u << r))) continue;
        const NkRenderFallbackEvent e = nk_render_fallback_window_event(&s_render_window,
                                                                        (NkRenderEffect)r);
        char line[256];
        if (nk_render_fallback_event_format(&e, line, sizeof(line)) > 0) sr_gui_boot_event("%s", line);
    }
}
#endif

#ifdef SR_SDL3VK
/* The window's keys and typed text go to the in-window keyboard while it is open. The text
 * input is claimed only then, so Escape opens the pause the rest of the time. */
static void sr_gui_feed_keyboard(void) {
    sdl3vk_set_text_claim(sr_osk_overlay_active());
    Sdl3VkKeyEvent ev[32];
    int n = sdl3vk_take_key_events(ev, 32);
    for (int i = 0; i < n; i++) {
        switch (ev[i].key) {
        case SDL3VK_KEY_UP:        sr_osk_overlay_key(OSK_KEY_UP); break;
        case SDL3VK_KEY_DOWN:      sr_osk_overlay_key(OSK_KEY_DOWN); break;
        case SDL3VK_KEY_LEFT:      sr_osk_overlay_key(OSK_KEY_LEFT); break;
        case SDL3VK_KEY_RIGHT:     sr_osk_overlay_key(OSK_KEY_RIGHT); break;
        case SDL3VK_KEY_ENTER:     sr_osk_overlay_key(OSK_KEY_SELECT); break;
        case SDL3VK_KEY_ESCAPE:    sr_osk_overlay_key(OSK_KEY_CANCEL); break;
        case SDL3VK_KEY_TAB:       sr_osk_overlay_key(OSK_KEY_CONFIRM); break;
        case SDL3VK_KEY_BACKSPACE: sr_osk_overlay_key(OSK_KEY_BACKSPACE); break;
        case SDL3VK_KEY_TEXT:      sr_osk_overlay_type(ev[i].codepoint); break;
        default: break;
        }
    }
}

/* Esc, F1 or Guide (or the open pad going away) asks the scheduler for a pause. The request
 * only latches: the pause opens at the scheduler's next switch point, and a refused request
 * leaves the game running (sched.c logs why). */
static void sr_gui_take_pause_request(void) {
    int request = sdl3vk_take_pause_request();
    if (request == SDL3VK_PAUSE_REQUEST_NONE || !sr_sched_on) return;
    (void)sched_pause_request(request == SDL3VK_PAUSE_REQUEST_PAD_LOST
                                  ? SR_PAUSE_SRC_PAD_LOST : SR_PAUSE_SRC_USER);
}

static void sync_sdl_input(void) {
    s_buttons = sdl3vk_buttons();
    sdl3vk_analog(&s_lx, &s_ly);
    {
        const NkInputProfile *prof = sdl3vk_get_input_profile();
        s_stick_dead_zone = prof ? ace_stick_host_counts(prof->axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner,
                                                         prof->axes[NK_PSP_AXIS_ANALOG_Y].deadzone_inner)
                                 : 0;
    }
    s_pad_present = sdl3vk_pad_present();
    sr_gui_feed_keyboard();
    sr_gui_take_pause_request();
}
#endif

#ifndef SR_GUI_PRESENT_SELFTEST
#if defined(SR_SDL3VK) || defined(_WIN32)
static bool sr_gui_is_headless_presenter(void) {
    const char *headless = getenv("SR_HEADLESS");
    if (headless && headless[0] && strcmp(headless, "0") != 0) return true;

    const char *driver = SDL_GetCurrentVideoDriver();
    return driver && (strcmp(driver, "dummy") == 0 ||
                      strcmp(driver, "offscreen") == 0);
}

#ifdef SR_SDL3VK
static void sr_gui_raise_sdl_window(void *context) {
    (void)context;
    (void)sdl3vk_raise_window();
}
#endif

#ifdef _WIN32
static void sr_gui_raise_gdi_window(void *context) {
    HWND window = (HWND)context;
    if (!window) return;
    BringWindowToTop(window);
    if (!SetForegroundWindow(window)) {
        fprintf(stderr, "gui_init: Win32 refused foreground activation for the game window\n");
    }
}
#endif

static void sr_gui_request_launcher_foreground(
    bool window_visible,
    void *context,
    SrGuiForegroundRequestFn raise_window,
    const char *backend
) {
    if (sr_gui_request_launcher_foreground_once(
            getenv("SR_BOOT_EVENT_FILE"), window_visible,
            sr_gui_is_headless_presenter(), &s_foreground_request_issued,
            context, raise_window)) {
        sr_gui_boot_event("BOOT_EVENT phase=window_foreground_requested backend=%s",
                          backend);
    }
}
#endif

/* PSP button bits (sceCtrl), named in src/core/nk_input_profile.h: SELECT 0x1, START 0x8,
 * UP 0x10, RIGHT 0x20, DOWN 0x40, LEFT 0x80, LTRIG 0x100, RTRIG 0x200, TRIANGLE 0x1000,
 * CIRCLE 0x2000, CROSS 0x4000, SQUARE 0x8000. One table, shared with the scripted-input
 * route language (src/rt/hle.c), so a live press and a scripted press cannot disagree. */
#ifdef _WIN32
static uint32_t read_keys(void) {
    uint32_t b = 0;
    #define K(vk,bit) do { if (GetAsyncKeyState(vk) & 0x8000) b |= (bit); } while (0)
    K(VK_RETURN, NK_PSP_BTN_START_BIT);         /* Enter  -> START  */
    K(VK_RSHIFT, NK_PSP_BTN_SELECT_BIT); K(VK_LSHIFT, NK_PSP_BTN_SELECT_BIT); /* Shift -> SELECT */
    K('X', NK_PSP_BTN_CROSS_BIT);               /* X -> CROSS   (confirm) */
    K('Z', NK_PSP_BTN_CIRCLE_BIT);              /* Z -> CIRCLE  (back)    */
    K('A', NK_PSP_BTN_SQUARE_BIT);              /* A -> SQUARE  */
    K('S', NK_PSP_BTN_TRIANGLE_BIT);            /* S -> TRIANGLE */
    K('Q', NK_PSP_BTN_LTRIGGER_BIT);            /* Q -> L */
    K('W', NK_PSP_BTN_RTRIGGER_BIT);            /* W -> R */
    K(VK_UP, NK_PSP_BTN_UP_BIT); K(VK_DOWN, NK_PSP_BTN_DOWN_BIT);
    K(VK_LEFT, NK_PSP_BTN_LEFT_BIT); K(VK_RIGHT, NK_PSP_BTN_RIGHT_BIT);
    #undef K
    return b;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}
#endif /* _WIN32: the keyboard fallback and the window class are Win32-only */

#if defined(SR_SDL3VK) || defined(_WIN32)
static void gui_release_presenter(void);

#if defined(SR_SDL3VK) && !defined(SR_GUI_PRESENT_SELFTEST)
/* ---- Ace Mode's in-game UI (ace.h, gpu_sdl3vk/ace_ui.h) ---------------------------------------
 * Made once at start in an Ace run that shows any of it; an authentic run never makes it and
 * never registers the presenter's UI pass. Without SDL3_ttf, the UI fonts or the Vulkan backend
 * the three features are off for the run, said once, and the keyboard keeps today's path. */
#define ACE_NOTICE_NS 3000000000ull              /* the launch notice stays 3 s */
static NkOverlayFonts *s_ace_fonts;
static AceUi *s_ace_ui;
static int s_ace_ui_ready;
static int s_ace_keyboard_now;                    /* this present's keyboard is the UI pass's */
static uint64_t s_ace_first_frame_ns;             /* the first accepted present: the notice starts */
static char s_ace_words[160];

static void gui_ace_ui_pass(const Sdl3VkUiPass *p) {
    const AceConfig *ace = ace_effective();
    AceUiFrame f;
    AceReadoutView view;
    uint64_t now = SDL_GetTicksNS();
    memset(&f, 0, sizeof(f));
    f.pixel_w = (int)p->width;
    f.pixel_h = (int)p->height;
    {
        float pixels_per_point = 1.0f;
        f.display_scale = 1.0f;
        (void)sdl3vk_window_scales(&f.display_scale, &pixels_per_point);
    }
    f.now_s = (double)now / 1e9;
    /* The player's Reduce motion choice, as the pause overlay reads it. */
    f.reduce_motion = nk_overlay_reduce_motion(getenv("SR_REDUCE_MOTION"),
                                               nk_platform_prefers_reduced_motion());
    /* The readout steps aside while the developer HUD shows. */
    if (ace->readout && sdl3vk_hud_mode() == SDL3VK_HUD_OFF && ace_readout_view(&view)) {
        f.readout = view.text;
        f.readout_warning = view.pc_limited;
    }
    if (ace->notice && s_ace_first_frame_ns && now - s_ace_first_frame_ns < ACE_NOTICE_NS) {
        double left = (double)(ACE_NOTICE_NS - (now - s_ace_first_frame_ns)) / 1e9;
        double shown = (double)(now - s_ace_first_frame_ns) / 1e9;
        f.notice_head = ace_tier_name();
        f.notice = s_ace_words[0] ? s_ace_words : "On";
        /* It eases in and out over NK_UI_MOTION_BASE, or cuts with Reduce motion. */
        f.notice_alpha = 1.0f;
        if (!f.reduce_motion) {
            double edge = shown < left ? shown : left;
            if (edge < NK_UI_MOTION_BASE) f.notice_alpha = (float)(edge / NK_UI_MOTION_BASE);
        }
    }
    if (s_ace_keyboard_now) f.keyboard = sr_osk_overlay_view();
    if (!ace_ui_frame_wants(&f)) return;
    AceUiTarget t;
    memset(&t, 0, sizeof(t));
    t.cmd = (VkCommandBuffer)p->cmd;
    t.frame_slot = p->frame_slot;
    t.format = (VkFormat)p->format;
    t.extent.width = p->width;
    t.extent.height = p->height;
    t.images = (const VkImage *)p->images;
    t.image_count = p->image_count;
    t.image_index = p->image_index;
    t.swapchain_gen = p->swapchain_gen;
    (void)ace_ui_record(s_ace_ui, &t, &f);
}

static void gui_ace_ui_off(const char *why) {
    char reason[200];
    snprintf(reason, sizeof(reason), "the in-game UI is unavailable: %s", why);
    if (ace_effective()->text) ace_feature_unavailable(ACE_FEATURE_TEXT, reason);
    if (ace_effective()->readout) ace_feature_unavailable(ACE_FEATURE_READOUT, reason);
    if (ace_effective()->notice) ace_feature_unavailable(ACE_FEATURE_NOTICE, reason);
}

static void gui_ace_ui_init(void) {
    const AceConfig *ace = ace_effective();
    char error[256] = "";
    Sdl3VkInfo vi;
    if (!g_ace_on || !(ace->text || ace->readout || ace->notice)) return;
    memset(&vi, 0, sizeof(vi));
    if (!sdl3vk_get_vk(&vi) || !vi.get_instance_proc_addr) {
        gui_ace_ui_off("no Vulkan device");
        return;
    }
    s_ace_fonts = nk_overlay_fonts_create(getenv("SR_UI_ROOT"), error, sizeof(error));
    if (s_ace_fonts) {
        AceUiInit init;
        memset(&init, 0, sizeof(init));
        init.get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)vi.get_instance_proc_addr;
        init.instance = (VkInstance)vi.instance;
        init.physical_device = (VkPhysicalDevice)vi.physical;
        init.device = (VkDevice)vi.device;
        init.queue = (VkQueue)vi.queue;
        init.queue_family = vi.queue_family;
        init.frames_in_flight = SDL3VK_PRESENT_FRAMES;   /* the pass checks every slot */
        init.text = nk_overlay_fonts_text(s_ace_fonts);
        s_ace_ui = ace_ui_create(&init, error, sizeof(error));
    }
    if (!s_ace_ui) {
        nk_overlay_fonts_destroy(s_ace_fonts);
        s_ace_fonts = NULL;
        gui_ace_ui_off(error[0] ? error : "unknown");
        return;
    }
    (void)ace_feature_words(s_ace_words, sizeof(s_ace_words));
    sdl3vk_set_ui_pass(gui_ace_ui_pass);
    s_ace_ui_ready = 1;
    ace_confirm(ACE_FEATURE_TEXT);
    ace_confirm(ACE_FEATURE_READOUT);
    ace_confirm(ACE_FEATURE_NOTICE);
}

/* ---- Ace Mode's smooth motion (ace.h, gpu_sdl3vk/ace_smooth.h) ----------------------------------
 * Made once at start in an Ace run that asks for it, and only then is the show pass registered,
 * so any other present records what it did before. Without compute the feature is off for the
 * run, said once. */
static void gui_ace_smooth_init(void) {
    char error[160] = "";
    Sdl3VkInfo vi;
    if (!g_ace_on || !ace_effective()->smooth) return;
    memset(&vi, 0, sizeof(vi));
    if (sdl3vk_get_vk(&vi) && vi.get_instance_proc_addr) {
        AceSmoothInit init;
        memset(&init, 0, sizeof(init));
        init.get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)vi.get_instance_proc_addr;
        init.instance = (VkInstance)vi.instance;
        init.physical_device = (VkPhysicalDevice)vi.physical;
        init.device = (VkDevice)vi.device;
        init.queue = (VkQueue)vi.queue;
        init.queue_family = vi.queue_family;
        s_ace_smooth = ace_smooth_create(&init, error, sizeof(error));
    }
    if (!s_ace_smooth) {
        ace_feature_unavailable(ACE_FEATURE_SMOOTH, error[0] ? error : "no Vulkan device");
        return;
    }
    ace_motion_queue_reset(&s_ace_motion_queue);
    sdl3vk_set_show_pass(gui_ace_show_pass);
    sdl3vk_set_show_done(gui_ace_show_done);
    /* The HUD mask and depth: on with smooth motion; SR_ACE_SMOOTH_HUD=0 and SR_ACE_SMOOTH_DEPTH=0
     * turn each off (ace_hud.h), and with both off the in-betweens are today's, byte for byte. */
    ace_hud_enable(1);
    ace_confirm(ACE_FEATURE_SMOOTH);
}

/* Widescreen Fill (#1881, wide=fill): the 3D scene continues into wings beside the PSP picture,
 * which gegpu_present (the GPU renderer) or gui_wide_compose (the software one) puts on either
 * side of the frame. Smooth motion's HUD mask and depth are sized to the 480-wide frame, so in
 * this build the two are exclusive: with smooth motion on, Fill reports itself unavailable. */
static int s_wide_on;

static void gui_cull_sync(void);

static void gui_wide_init(void) {
    const AceConfig *effective = ace_effective();
    s_wide_on = 0;
    if (!g_ace_on || effective->wide != ACE_WIDE_FILL) return;
    if (ace_wide_fill_blocked_by_smooth(effective->wide == ACE_WIDE_FILL,
                                        effective->smooth != ACE_SMOOTH_OFF)) {
        ace_feature_unavailable(ACE_FEATURE_WIDE, "smooth motion is on, and its HUD mask and depth are "
                                                  "sized to the 480-wide frame");
        return;
    }
    s_wide_on = 1;
    ace_confirm(ACE_FEATURE_WIDE);
    ge_wide_set_cull(effective->cull);
    ace_set_finish_hook(gui_cull_sync);   /* the exit path reads the final count (contract 11.3.6) */
}

static void gui_cull_sync(void) {
    /* Confirmed only while no matrix was left alone; a full table is a partial run (contract 11.3.6). */
    ace_set_cull_untracked(ge_wide_cull_untracked());
    if (ge_wide_cull_untracked())
        ace_feature_unavailable(ACE_FEATURE_CULL,
                                "partial: the 4096-matrix table filled, later matrices were left alone");
    else if (ge_wide_cull_calls())
        ace_confirm(ACE_FEATURE_CULL);   /* the title's hook has run */
}

static void gui_wide_release(void) {
    gui_cull_sync();
    ace_set_finish_hook(NULL);
    if (s_wide_on) (void)ge_wide_set_wing(0);
    ge_wide_set_cull(0);
    s_wide_on = 0;
}

static unsigned long long s_fsr1_presents;  /* presents the upscaler made, said at shutdown */

/* An FSR 1 pass on the presenter's device, or NULL with the reason in error. */
static Fsr1Pass *gui_fsr1_make(char *error, size_t error_size) {
    Sdl3VkInfo vi;
    memset(&vi, 0, sizeof(vi));
    if (!sdl3vk_get_vk(&vi) || !vi.get_instance_proc_addr) {
        snprintf(error, error_size, "no Vulkan device");
        return NULL;
    }
    Fsr1Init init;
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)vi.get_instance_proc_addr;
    init.instance = (VkInstance)vi.instance;
    init.physical_device = (VkPhysicalDevice)vi.physical;
    init.device = (VkDevice)vi.device;
    init.queue_family = vi.queue_family;
    return fsr1_create(&init, error, error_size);
}

/* An Anime4K pass on the presenter's device, or NULL with the reason in error. */
static Anime4kPass *gui_anime4k_make(char *error, size_t error_size) {
    Sdl3VkInfo vi;
    memset(&vi, 0, sizeof(vi));
    if (!sdl3vk_get_vk(&vi) || !vi.get_instance_proc_addr) {
        snprintf(error, error_size, "no Vulkan device");
        return NULL;
    }
    Anime4kInit init;
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)vi.get_instance_proc_addr;
    init.instance = (VkInstance)vi.instance;
    init.physical_device = (VkPhysicalDevice)vi.physical;
    init.device = (VkDevice)vi.device;
    init.queue_family = vi.queue_family;
    return anime4k_create(&init, error, error_size);
}

/* Makes a movie set's passes, once. Said when one is missing; the set works with the other. */
static int gui_movie_make(MovieUpscale *m, const char *where) {
    if (m->anime4k || m->fsr1) return 1;
    if (m->failed) return 0;
    char error[160] = "";
    if (s_movie_mode == MOVIE_UPSCALE_ANIME4K) {
        m->anime4k = gui_anime4k_make(error, sizeof(error));
        if (!m->anime4k)
            fprintf(stderr, "sdl3vk: Anime4K is unavailable for %s (%s); its movies use FSR 1\n", where,
                    error[0] ? error : "unknown");
    }
    error[0] = '\0';
    m->fsr1 = gui_fsr1_make(error, sizeof(error));
    if (!m->fsr1 && !m->anime4k) {
        m->failed = 1;
        fprintf(stderr, "sdl3vk: movie upscaling is unavailable for %s (%s)\n", where,
                error[0] ? error : "unknown");
        return 0;
    }
    return 1;
}

/* A CPU-presented frame under SR_MOVIE_UPSCALE: Anime4K's 2x stages while the picture is well below
 * its place (anime4k), then FSR 1 to the exact size; FSR 1 alone for fsr1, or when Anime4K is
 * unavailable. NULL leaves the frame to the presentation upscaler or the scaling. */
static VkImage gui_movie_record(MovieUpscale *m, VkCommandBuffer cmd, VkImage src, int w, int h,
                                int out_w, int out_h, int *anime4k) {
    VkImage in = src;
    int iw = w, ih = h;
    if (m->anime4k) {
        int stages = anime4k_stages_for(w, h, out_w, out_h);
        VkImage doubled = stages ? anime4k_record(m->anime4k, cmd, src, w, h, stages) : VK_NULL_HANDLE;
        if (doubled) {
            in = doubled;
            iw = w << stages;
            ih = h << stages;
        }
    }
    VkImage out = in;
    if (in == src || iw != out_w || ih != out_h) {
        /* FSR 1 takes the picture to the exact size, unless Anime4K's doubling already is it. */
        if (!m->fsr1) return VK_NULL_HANDLE;
        out = fsr1_record(m->fsr1, cmd, in, iw, ih, out_w, out_h, FSR1_SHARPNESS_DEFAULT);
    }
    if (out && in != src) {
        m->anime4k_presents++;
        if (anime4k) *anime4k = 1;
    }
    return out;
}

static void *gui_upscale_pass(void *cmd, void *src, int w, int h, int out_w, int out_h, int cpu_frame,
                              int *anime4k) {
    if (cpu_frame && s_movie_mode != MOVIE_UPSCALE_OFF) {
        VkImage movie = gui_movie_record(&s_movie, (VkCommandBuffer)cmd, (VkImage)src, w, h, out_w, out_h,
                                         anime4k);
        if (movie) {
            s_movie.presents++;
            return (void *)movie;
        }
    }
    if (!s_fsr1) return NULL;
    VkImage out = fsr1_record(s_fsr1, (VkCommandBuffer)cmd, (VkImage)src, w, h, out_w, out_h,
                              FSR1_SHARPNESS_DEFAULT);
    if (out) s_fsr1_presents++;
    return (void *)out;
}

static unsigned long long s_fsr1_rec_presents;  /* recorded presents the upscaler made */

/* The recording's upscale (sdl3vk_set_record_upscale_pass): its own pass, so the window's size and
 * the recording's never make one rebuild its images for the other. Without it the recording is
 * scaled as SR_PRESENT_SCALING says, and record.json names the blits that made it. */
static void *gui_record_upscale_pass(void *cmd, void *src, int w, int h, int out_w, int out_h,
                                     int cpu_frame, int *anime4k) {
    if (cpu_frame && s_movie_mode != MOVIE_UPSCALE_OFF && gui_movie_make(&s_movie_rec, "the recording")) {
        VkImage movie = gui_movie_record(&s_movie_rec, (VkCommandBuffer)cmd, (VkImage)src, w, h, out_w,
                                         out_h, anime4k);
        if (movie) {
            s_movie_rec.presents++;
            return (void *)movie;
        }
    }
    if (!s_fsr1 && !s_fsr1_rec) return NULL;
    if (!s_fsr1_rec && !s_fsr1_rec_failed) {
        char error[160] = "";
        s_fsr1_rec = gui_fsr1_make(error, sizeof(error));
        if (!s_fsr1_rec) {
            s_fsr1_rec_failed = 1;
            fprintf(stderr, "sdl3vk: FSR 1 is unavailable for the recording (%s); it is scaled as "
                            "SR_PRESENT_SCALING says, and record.json names the blits that made it\n",
                    error[0] ? error : "unknown");
        }
    }
    if (!s_fsr1_rec) return NULL;
    VkImage out = fsr1_record(s_fsr1_rec, (VkCommandBuffer)cmd, (VkImage)src, w, h, out_w, out_h,
                              FSR1_SHARPNESS_DEFAULT);
    if (out) s_fsr1_rec_presents++;
    return (void *)out;
}

/* SR_PRESENT_UPSCALE=fsr1: AMD FSR 1 (EASU + RCAS) scales the shown picture to the window.
 * SR_MOVIE_UPSCALE=fsr1|anime4k: CPU-presented frames (decoded movies) get their own upscaler,
 * Anime4K's CNN stages then FSR 1, whatever SR_PRESENT_UPSCALE says. Both are presentation only:
 * the guest's frame, the decoded movie, guest VRAM and captures are untouched. Both are off by
 * default. */
static void gui_upscale_init(void) {
    const char *value = getenv("SR_PRESENT_UPSCALE");
    int mode = fsr1_parse_mode(value);
    if (mode < 0)
        fprintf(stderr, "sdl3vk: unknown SR_PRESENT_UPSCALE '%s' (off or fsr1); upscaling stays off\n",
                value);
    const char *movie = getenv("SR_MOVIE_UPSCALE");
    s_movie_mode = anime4k_parse_movie_mode(movie);
    if (s_movie_mode < 0) {
        fprintf(stderr, "sdl3vk: unknown SR_MOVIE_UPSCALE '%s' (off, fsr1 or anime4k); movie upscaling "
                "stays off\n", movie);
        s_movie_mode = MOVIE_UPSCALE_OFF;
    }
    if (mode != 1 && s_movie_mode == MOVIE_UPSCALE_OFF) return;
    char error[160] = "";
    if (mode == 1) {
        s_fsr1 = gui_fsr1_make(error, sizeof(error));
        if (!s_fsr1)
            fprintf(stderr, "sdl3vk: FSR 1 upscaling is unavailable (%s); the window scales as before\n",
                    error[0] ? error : "no Vulkan device");
    }
    if (s_movie_mode != MOVIE_UPSCALE_OFF && !gui_movie_make(&s_movie, "the window"))
        s_movie_mode = MOVIE_UPSCALE_OFF;
    if (!s_fsr1 && s_movie_mode == MOVIE_UPSCALE_OFF) return;
    sdl3vk_set_upscale_pass(gui_upscale_pass);
    /* A recording (SR_VIDEOREC_DIR) is enlarged the same way, at its own size (#2138). */
    sdl3vk_set_record_upscale_pass(gui_record_upscale_pass);
    /* Stamped like Ace Mode: the window is not the authentic picture (captures still are). */
    /* The stamp names what runs: a movie upscaler asked for Anime4K but made without it is FSR 1. The
     * longest is "fsr1+movie-anime4k"; flight_recorder.c's buffer is sized for it. */
    const char *stamp = s_fsr1 ? (s_movie_mode == MOVIE_UPSCALE_OFF ? "fsr1"
                                  : s_movie.anime4k ? "fsr1+movie-anime4k" : "fsr1+movie-fsr1")
                               : (s_movie.anime4k ? "movie-anime4k" : "movie-fsr1");
    sr_flight_set_present_upscale(stamp);
    fprintf(stderr, "ACE_MODE: present upscale %s (the window and its recording; captures hold "
                    "the guest's frame)\n", stamp);
    /* In the boot events too, so tools/boot_gate.py sees it when stderr was silenced. */
    sr_gui_boot_event("BOOT_EVENT phase=present_upscale mode=%s", stamp);
    if (s_fsr1) fprintf(stderr, "sdl3vk: FSR 1 upscaling on (EASU + RCAS, presentation only)\n");
    if (s_movie_mode != MOVIE_UPSCALE_OFF)
        fprintf(stderr, "sdl3vk: movie upscaling on (%s, presentation only)\n",
                s_movie.anime4k ? "Anime4K CNN x2 + FSR 1" : "FSR 1");
}

static void gui_movie_release(MovieUpscale *m, const char *what) {
    if (m->presents)
        fprintf(stderr, "sdl3vk: the movie upscaler made %llu %spresents, %llu through Anime4K\n",
                m->presents, what, m->anime4k_presents);
    anime4k_destroy(m->anime4k);
    fsr1_destroy(m->fsr1);
    memset(m, 0, sizeof(*m));
}

static void gui_upscale_release(void) {
    if (s_fsr1 || s_movie_mode != MOVIE_UPSCALE_OFF) {
        sdl3vk_set_upscale_pass(NULL);
        sdl3vk_set_record_upscale_pass(NULL);
    }
    if (s_fsr1) {
        fprintf(stderr, "sdl3vk: FSR 1 upscaled %llu presents\n", s_fsr1_presents);
        if (s_fsr1_rec)
            fprintf(stderr, "sdl3vk: FSR 1 upscaled %llu recorded presents\n", s_fsr1_rec_presents);
    }
    gui_movie_release(&s_movie, "");
    gui_movie_release(&s_movie_rec, "recorded ");
    s_movie_mode = MOVIE_UPSCALE_OFF;
    fsr1_destroy(s_fsr1_rec);
    s_fsr1_rec = NULL;
    s_fsr1_rec_failed = 0;
    fsr1_destroy(s_fsr1);
    s_fsr1 = NULL;
}

static void gui_ace_smooth_release(void) {
    if (s_ace_smooth) {
        sdl3vk_set_show_pass(NULL);
        sdl3vk_set_show_done(NULL);
    }
    ace_hud_enable(0);
    ace_smooth_destroy(s_ace_smooth);
    s_ace_smooth = NULL;
}

static void gui_ace_ui_release(void) {
    if (s_ace_ui_ready) sdl3vk_set_ui_pass(NULL);
    s_ace_ui_ready = 0;
    ace_ui_destroy(s_ace_ui);
    s_ace_ui = NULL;
    nk_overlay_fonts_destroy(s_ace_fonts);
    s_ace_fonts = NULL;
}
#endif

#endif

/* Releases what gui_init set up: called by the driver once the scheduler has returned, and at
 * process exit for a run that ends any other way (sceKernelExitGame's exit()). The SR_PERF
 * summary is written first, while the GPU GE it samples still exists; each step clears what it
 * released, so a second call does nothing (#1072, #1377). */
void gui_shutdown(void) {
#if defined(SR_SDL3VK) || defined(_WIN32)
    sr_perf_shutdown();
    gui_release_presenter();
#endif
}

/* The render path this run set up, from what it asked for and what started, reported once
 * before window_ready (#2151): the player reads it from the boot events and names a slower
 * path than the one asked for. */
#if defined(SR_SDL3VK) || defined(_WIN32)
static void gui_report_render_path(bool vulkan_asked, bool vulkan_started, bool gpu_ge_asked,
                                   bool gpu_ge_started) {
    NkRenderPath path = nk_render_path_from_init(vulkan_asked, vulkan_started, gpu_ge_asked,
                                                 gpu_ge_started);
    char line[128];
    if (nk_render_path_event_format(&path, line, sizeof(line)) > 0) {
        sr_gui_boot_event("%s", line);
    }
}
#endif

void gui_init(const char *title) {
    (void)title;
#if defined(SR_SDL3VK) || defined(_WIN32)
    static int cleanup_registered;
    if (!cleanup_registered && atexit(gui_shutdown) == 0) cleanup_registered = 1;
#endif
    gui_headless_acceptance_parse();
    /* An Ace Mode run (ace.h) says so in the structured boot events too, so a log whose stderr
     * was silenced (SR_QUIET) still tells tools/boot_gate.py it is not authentic evidence. */
    if (g_ace_on) sr_gui_boot_event("BOOT_EVENT phase=ace_mode on=1");
    if (s_headless_accept_requested && s_headless_accept_ace) {
        sr_gui_boot_event("BOOT_EVENT phase=headless_acceptance status=FAIL reason=ace_mode");
        return;
    }
    if (s_headless_accept_requested && s_headless_accept_invalid) {
        sr_gui_boot_event("BOOT_EVENT phase=headless_acceptance status=FAIL reason=invalid_frame_target");
        return;
    }
    const char *video = getenv("SR_VIDEO");
    if (s_headless_accept_requested && video &&
        strcmp(video, "offscreen") == 0) {
        sr_gui_boot_event("BOOT_EVENT phase=headless_acceptance status=FAIL reason=offscreen_presenter");
        return;
    }
    const char *sdl_video_driver = getenv("SDL_VIDEODRIVER");
    const char *sdl_video_driver_alias = getenv("SDL_VIDEO_DRIVER");
    if (s_headless_accept_requested &&
        ((sdl_video_driver && strcmp(sdl_video_driver, "offscreen") == 0) ||
         (sdl_video_driver_alias && strcmp(sdl_video_driver_alias, "offscreen") == 0))) {
        sr_gui_boot_event("BOOT_EVENT phase=headless_acceptance status=FAIL reason=offscreen_sdl_driver");
        return;
    }
    if (video && strcmp(video, "offscreen") == 0) {
        /* The offscreen presenter is an explicit host-memory sink. It does not
         * initialize SDL video, create a window, or claim Vulkan presentation;
         * gui_present accepts a frame only after conversion has written this
         * allocated sink. Interactive routes keep their existing selection. */
        s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
        if (!s_px) {
            fprintf(stderr, "gui_init: offscreen presenter allocation failed\n");
            sr_gui_boot_event("BOOT_EVENT phase=window_unavailable backend=offscreen");
            return;
        }
        s_offscreen = 1;
        s_offscreen_frames = 0;
        s_last_ns = SDL_GetTicksNS();
        s_on = 1;
        /* window_ready is the established boot-gate name for the host-presenter
         * initialization milestone. The backend field makes the no-window case
         * explicit; this event does not claim that an OS window exists. */
        sr_gui_boot_event("BOOT_EVENT phase=window_ready backend=offscreen");
        return;
    }
    /* The GPU GE renderer is asked for by SR_GPU_GE (the player always sets it). */
    const char *gpu_ge_env = getenv("SR_GPU_GE");
    bool gpu_ge_asked = gpu_ge_env && gpu_ge_env[0] && strcmp(gpu_ge_env, "0") != 0;
    bool vulkan_asked = false;
    (void)gpu_ge_asked;   /* read only by the presenters this build has */
    (void)vulkan_asked;
#ifdef SR_SDL3VK
    /* SDL3+Vulkan presenter (src/rt/gpu_sdl3vk, Phase 0): default in this build;
     * SR_VIDEO=gdi falls back to the classic Win32/GDI window below. */
    {
        const char *v = getenv("SR_VIDEO");
        if (!v || strcmp(v, "gdi") != 0) {
            vulkan_asked = true;
            if (sdl3vk_init(title)) {
#ifdef SR_PUBLIC_SAFE
                /* Only the public audio backend exports this query; without a
                   callback the HUD reports audio status as unknown. */
                extern int sr_audio_is_active(void);
                sdl3vk_set_audio_active_cb(sr_audio_is_active);
#endif
                sdl3vk_set_logic_slowed_cb(sr_enhancement_logic_slowed);
                s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
                if (!s_px) {
                    /* No frame copy, so nothing can be presented: the presenter just created
                     * is released again rather than left half set up (#1377). */
                    fprintf(stderr, "gui_init: frame copy allocation failed\n");
                    sdl3vk_shutdown();
                    sr_gui_boot_event("BOOT_EVENT phase=window_unavailable backend=vulkan");
                    return;
                }
                s_sdl3 = 1;
                s_last_ns = SDL_GetTicksNS();
                s_on = 1;
                if (s_headless_accept_requested) s_headless_accept_ready = 1;
                /* The window draws the on-screen keyboard, so no native box is needed; a
                 * presenter that cannot draw keeps the box as the fallback (osk_text_entry.c). */
                sr_osk_overlay_set_host(osk_overlay_paint_available());
#ifndef SR_GUI_PRESENT_SELFTEST
                gui_ace_ui_init();
                gui_ace_smooth_init();
                gui_wide_init();   /* after smooth motion, which it steps aside for */
                gui_upscale_init();
#endif
                sync_sdl_input();
                /* Phase 1 GPU rasterizer (opt-in): captures GE triangles/sprites and
                 * renders them on the GPU, writing results back to guest VRAM. */
                {
                    bool gpu_ge_started = false;
                    if (gpu_ge_asked) {
                        gpu_ge_started = gegpu_init() != 0;
                        if (!gpu_ge_started)
                            fprintf(stderr, "gui_init: GPU GE init failed; software GE active\n");
                    }
                    gui_report_render_path(true, true, gpu_ge_asked, gpu_ge_started);
                }
                sr_gui_request_launcher_foreground(true, NULL,
                                                   sr_gui_raise_sdl_window,
                                                   "vulkan");
                sr_gui_boot_event("BOOT_EVENT phase=window_ready backend=vulkan");
                return;
            }
#ifdef _WIN32
            fprintf(stderr, "gui_init: SDL3/Vulkan init failed; falling back to GDI\n");
#else
            fprintf(stderr, "gui_init: SDL3/Vulkan init failed\n");
#endif
        }
    }
#endif
#ifdef _WIN32
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "psp_recomp";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, PSP_W * 2, PSP_H * 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    /* Centred on the monitor that holds the launcher window, as the SDL presenter does
     * (SR_LAUNCHER_DISPLAY_BOUNDS, recomp.h); without a usable value Windows places it. */
    int win_x = CW_USEDEFAULT, win_y = CW_USEDEFAULT;
    {
        const char *value = getenv("SR_LAUNCHER_DISPLAY_BOUNDS");
        int b[4];
        if (value && value[0] && !sr_parse_display_bounds(value, b)) {
            fprintf(stderr, "gui_init: SR_LAUNCHER_DISPLAY_BOUNDS=%s is not x,y,w,h; "
                            "opening on the primary display\n", value);
        } else if (value && value[0]) {
            POINT centre = { b[0] + b[2] / 2, b[1] + b[3] / 2 };
            HMONITOR mon = MonitorFromPoint(centre, MONITOR_DEFAULTTONULL);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            if (!mon) {
                fprintf(stderr, "gui_init: no monitor contains the launcher's display %s; "
                                "opening on the primary display\n", value);
                mon = MonitorFromPoint(centre, MONITOR_DEFAULTTOPRIMARY);
            }
            if (GetMonitorInfoA(mon, &mi)) {
                win_x = mi.rcWork.left +
                        ((mi.rcWork.right - mi.rcWork.left) - (r.right - r.left)) / 2;
                win_y = mi.rcWork.top +
                        ((mi.rcWork.bottom - mi.rcWork.top) - (r.bottom - r.top)) / 2;
            }
        }
    }
    s_hwnd = CreateWindowA("psp_recomp", title ? title : SR_APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, win_x, win_y,
        r.right - r.left, r.bottom - r.top, 0, 0, GetModuleHandleA(0), 0);
    s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
    if (!s_px) {
        fprintf(stderr, "gui_init: frame copy allocation failed\n");
        if (s_hwnd) DestroyWindow(s_hwnd);
        s_hwnd = NULL;
        sr_gui_boot_event("BOOT_EVENT phase=window_unavailable backend=gdi");
        return;
    }
    s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth = PSP_W;
    s_bmi.bmiHeader.biHeight = -PSP_H;          /* top-down */
    s_bmi.bmiHeader.biPlanes = 1;
    s_bmi.bmiHeader.biBitCount = 32;
    s_bmi.bmiHeader.biCompression = BI_RGB;
    s_last_ns = SDL_GetTicksNS();
    s_on = 1;
    if (s_headless_accept_requested && s_hwnd) s_headless_accept_ready = 1;
    sr_gui_request_launcher_foreground(
        s_hwnd && IsWindowVisible(s_hwnd), s_hwnd,
        sr_gui_raise_gdi_window, "gdi");
    /* Here when SR_VIDEO=gdi asked for this window, or when Vulkan did not start. */
    gui_report_render_path(vulkan_asked, false, gpu_ge_asked, false);
    sr_gui_boot_event("BOOT_EVENT phase=window_ready backend=gdi");
#else /* !_WIN32: no GDI presenter on this host */
    /* No Win32/GDI window exists here, so there is no fallback to take. The
     * refusal is reported and s_on stays 0: the runtime keeps executing the
     * guest and the framebuffer capture/diagnostics keep working, but nothing
     * claims a window that was never created. */
    fprintf(stderr,
            "gui_init: SDL3/Vulkan init failed and no Win32/GDI presenter exists on "
            "this host; running without a window (framebuffer capture still works)\n");
    s_on = 0;
    sr_gui_boot_event("BOOT_EVENT phase=window_unavailable backend=none");
#endif /* _WIN32 */
}

int gui_on(void) { return s_on; }
/* 1 only while a person is looking at a real host window: a presenter is up and it is not the
 * offscreen sink, a headless/dummy-video-driver host, or a headless acceptance launch. Those
 * unattended presenters also report gui_on(), but nobody there can press START, so the
 * runtime's bootstrap input (h_CtrlButtons) must keep running for them. */
int gui_interactive(void) {
    if (!s_on || s_offscreen || s_headless_accept_requested) return 0;
#if defined(SR_SDL3VK) || defined(_WIN32)
    if (sr_gui_is_headless_presenter()) return 0;
#endif
    return 1;
}
#endif /* !SR_GUI_PRESENT_SELFTEST */
#ifdef SR_GUI_INPUT_PATH
uint32_t gui_buttons(void) { return s_buttons; }
void gui_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = s_lx; if (ly) *ly = s_ly; }
int gui_stick_dead_zone(void) { return s_stick_dead_zone; }
int gui_pad_present(void) { return s_pad_present; }
void gui_consume_button_pulses(void) {
#ifdef SR_SDL3VK
    if (s_sdl3) {
        sdl3vk_consume_button_pulses();
        sync_sdl_input();
    }
#endif
}
#endif /* SR_GUI_INPUT_PATH */
#ifndef SR_GUI_PRESENT_SELFTEST

/* Gamepad presses since the last call (osk_overlay.h). The keyboard takes them every VBLANK,
 * so the list never builds up while it is closed. Returns 0 when no SDL3 presenter is up: the
 * GDI and offscreen presenters have no SDL3 gamepad. The overlay host governs drawing, not this
 * read; the presses are drained whether or not the host is on. */
uint32_t gui_pad_pulses_take(void) {
#ifdef SR_SDL3VK
    if (s_sdl3) return sdl3vk_take_pad_pulses();
#endif
    return 0u;
}

/* Present a framebuffer at guest address fbaddr. fmt: 0=5650, 1=5551, 2=4444, 3=8888.
 * stride is in pixels (PSP buffer width, typically 512). */
/* Convert the guest framebuffer to the BGRA words both presenters consume. The
 * offscreen sink converts too, so this is not behind a presenter #if. */
/* One framebuffer word (a 16-bit format in the low half) as the BGRA word the presenters take. */
static uint32_t bgra_from_fb_word(uint32_t p, int fmt) {
    int rr, gg, bb;
    if (fmt == 3) {
        rr = p & 0xFF; gg = (p >> 8) & 0xFF; bb = (p >> 16) & 0xFF;
    } else if (fmt == 1) {     /* 5551 */
        rr = (p & 0x1F) << 3; gg = ((p >> 5) & 0x1F) << 3; bb = ((p >> 10) & 0x1F) << 3;
    } else if (fmt == 2) {     /* 4444 */
        rr = (p & 0xF) << 4; gg = ((p >> 4) & 0xF) << 4; bb = ((p >> 8) & 0xF) << 4;
    } else {                   /* 5650 */
        rr = (p & 0x1F) << 3; gg = ((p >> 5) & 0x3F) << 2; bb = ((p >> 11) & 0x1F) << 3;
    }
    return ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
}

static void convert_fb_pixels(uint32_t fbaddr, int fmt, uint32_t stride) {
    for (int y = 0; y < PSP_H; y++) {
        /* Enhanced 32-bit colour (#2050): the software renderer's host twin gives the row when
         * it is on (RGBA8); off, and always under the Vulkan renderer, it returns 0. */
        uint32_t *row = s_px + y * PSP_W;
        if (ge_twin_present_row(fbaddr, fmt, stride, y, row, PSP_W)) {
            for (int x = 0; x < PSP_W; x++) {
                const uint32_t c = row[x];
                row[x] = ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
            }
            continue;
        }
        for (int x = 0; x < PSP_W; x++) {
            uint32_t i = (uint32_t)(y * (int)stride + x);
            uint32_t p;
            if (fmt == 3) {
                /* display_host_span_valid checks the complete framebuffer before this
                 * conversion. Read framebuffer storage literally: sr_r32 reserves the
                 * GE status port at 0x04084000, which can otherwise replace a pixel when
                 * a PSP VRAM framebuffer spans that address. */
                memcpy(&p, SR_HOST(fbaddr + i * 4), sizeof p);
            } else {
                p = sr_r16(fbaddr + i * 2);
            }
            s_px[y * PSP_W + x] = bgra_from_fb_word(p, fmt);
        }
    }
}

#if defined(SR_SDL3VK) && !defined(SR_GUI_PRESENT_SELFTEST)
/* Each present with Fill on: the wing width follows the window's shape (a change clears the
 * canvases), and the shown display buffer keeps a canvas. */
static void gui_wide_update(uint32_t fbaddr) {
    int w = 0, h = 0;
    if (!s_wide_on) return;
    if (sdl3vk_window_pixel_size(&w, &h)) (void)ge_wide_set_wing(ge_wide_wing_for(w, h));
    gui_cull_sync();
    ge_wide_note_display(fbaddr);
}

/* The software renderer's Fill picture in s_wide_px (ge_wide_picture): the left wing, s_px
 * (the frame, with anything drawn over it), then the right wing, converted like the frame.
 * Wings the shown frame did not draw are black, never an earlier frame's; so are the wings of a
 * frame the GPU renderer drew and this path presents (its canvases are on the GPU). Returns the
 * picture's width, or 0 when Fill is off and the frame is presented alone. */
static uint32_t s_wide_px[SDL3VK_UPLOAD_MAX_W * PSP_H];

static int gui_wide_compose(uint32_t fbaddr) {
    if (!s_wide_on) return 0;
    return ge_wide_picture(fbaddr, s_px, s_wide_px, bgra_from_fb_word);
}
#endif

/* A guest frame: the conversion plus the first-frame boot milestone. */
static void convert_fb(uint32_t fbaddr, int fmt, uint32_t stride) {
    convert_fb_pixels(fbaddr, fmt, stride);
    static int s_first_frame = 1;
    if (s_first_frame) {
        s_first_frame = 0;
        unsigned long nz = 0;
        for (int yy = 0; yy < PSP_H; yy++)
            for (int xx = 0; xx < PSP_W; xx++)
                if (s_px[yy * PSP_W + xx] & 0x00FFFFFF) nz++;
        fprintf(stderr, "[FIRST_FRAME] non_zero_pixels=%lu / total=%u\n", nz, PSP_W * PSP_H);
        sr_gui_boot_event("BOOT_EVENT phase=first_frame source=cpu nonzero_pixels=%lu total_pixels=%u",
                nz, PSP_W * PSP_H);
        if (getenv("SR_FIRST_FRAME_DUMP")) {
            FILE *raw = fopen("vram_first.bin", "wb");
            if (raw) {
                for (uint32_t a = 0x04000000; a < 0x04000000u + 480u * 272u * 2u; a++)
                    fputc((int)sr_r8(a), raw);
                fclose(raw);
                fprintf(stderr, "[FIRST_FRAME] dumped VRAM 0x04000000 -> vram_first.bin\n");
            }
            FILE *ppm = fopen("frame_first.ppm", "wb");
            if (ppm) {
                fprintf(ppm, "P6\n480 272\n255\n");
                for (int yy = 0; yy < PSP_H; yy++)
                    for (int xx = 0; xx < PSP_W; xx++) {
                        uint32_t p = s_px[yy * PSP_W + xx];
                        unsigned char rgb[3] = { (unsigned char)((p >> 16) & 0xFF),
                                                (unsigned char)((p >> 8) & 0xFF),
                                                (unsigned char)(p & 0xFF) };
                        fwrite(rgb, 1, 3, ppm);
                    }
                fclose(ppm);
                fprintf(stderr, "[FIRST_FRAME] dumped BGRA -> frame_first.ppm\n");
            }
        }
    }
}

/* ---- quit (#1377, #1392) ---------------------------------------------------------------
 * Every way the person asks to quit goes through gui_quit_request: the window's close (SDL3,
 * or a Win32 WM_QUIT), a presenter that reports the window closed, and the pause overlay's
 * Quit or a close while paused. Nothing here exits the process. The first request notifies the
 * guest's registered exit callback (hle.c); once sr_exit_request_stop_due says the run should
 * end, gui_stop_requested reports it to the scheduler (the driver polls it through
 * sr_sched_stop_requested), which stops at its next boundary. sched_run then returns to the
 * driver, which writes the SR_PERF summary, releases the presenter (gui_shutdown) and returns
 * from main, so every stdio stream is flushed and every atexit handler runs. The guest threads
 * are coroutines left suspended at a scheduler boundary; no atexit handler resumes one. */
#endif /* !SR_GUI_PRESENT_SELFTEST */
#ifdef SR_GUI_INPUT_PATH
static int s_quit_requested;

#if defined(SR_SDL3VK) || defined(_WIN32)   /* the presenters that have a window to close */
static void gui_quit_request(void) {
    if (!s_quit_requested) {
        s_quit_requested = 1;
        sr_gui_boot_event("BOOT_EVENT phase=quit_requested");
    }
    (void)sr_exit_request_stop_due();
}
#endif

int gui_quit_requested(void) {
    return s_quit_requested;
}
#endif /* SR_GUI_INPUT_PATH */

#ifndef SR_GUI_PRESENT_SELFTEST
static int s_quit_stop_reported;

int gui_stop_requested(void) {
    if (gui_headless_acceptance_complete()) return 1;
    if (!s_quit_requested || !sr_exit_request_stop_due()) return 0;
    if (!s_quit_stop_reported) {
        s_quit_stop_reported = 1;
        fprintf(stderr, "SR_QUIT: the run stops at the scheduler's next boundary\n");
        fflush(stderr);
    }
    return 1;
}

void gui_pump(void) {
    if (!s_on) return;
    if (s_offscreen) return;
#ifdef SR_SDL3VK
    if (s_sdl3) {
        if (!sdl3vk_poll()) gui_quit_request();
        sync_sdl_input();
        return;
    }
#endif
#ifdef _WIN32
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { gui_quit_request(); continue; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
#endif
}

#ifdef _WIN32
/* Blit the converted frame (s_px) into the GDI window. 1 = the frame was drawn. */
static int gdi_blit(void) {
    if (!s_hwnd) return 0;
    HDC dc = GetDC(s_hwnd);
    if (!dc) return 0;
    RECT cr;
    if (!GetClientRect(s_hwnd, &cr)) {
        ReleaseDC(s_hwnd, dc);
        return 0;
    }
    int scanlines = StretchDIBits(dc, 0, 0, cr.right, cr.bottom, 0, 0, PSP_W, PSP_H,
                                  s_px, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(s_hwnd, dc);
    return scanlines > 0;
}
#endif

/* CPU-backed presenters consume guest memory, so cross the same lazy GE read
 * barrier as the scalar accessors before converting a complete framebuffer. The
 * direct Vulkan present path bypasses this helper and keeps its target resident. */
static int display_cpu_source_current(uint32_t fbaddr, int fmt, uint32_t stride) {
    if (fmt < 0 || fmt > 3 || stride == 0u) return 0;
    uint32_t row_bytes, total_bytes;
    uint32_t bytes_per_pixel = fmt == 3 ? 4u : 2u;
    if (!sr_size_mul_ok(stride, bytes_per_pixel, &row_bytes) ||
        !sr_size_mul_ok(row_bytes, PSP_H, &total_bytes)) return 0;
    return sr_guest_span_readable_current(fbaddr, total_bytes);
}

#endif /* !SR_GUI_PRESENT_SELFTEST */

#ifdef SR_GUI_INPUT_PATH
/* The VBLANK after an in-between: its held frame goes to the screen. Nothing of the guest runs
 * and no capture can be armed here, and the VBLANK's timing is the host's present cost, like any
 * other. The present reads the window's events, so, as after every present, they go to the
 * game's input state before its controller sample later in this VBLANK reads it (a tap seen
 * only by the presenter would be cleared unread), and a closed window requests quit. Off,
 * ace_smooth_vblank says no. */
void gui_ace_vblank(uint32_t vcount) {
#ifdef SR_SDL3VK
    if (!ace_smooth_vblank(vcount)) return;
    if (!s_ace_smooth || s_quit_requested || !s_on || !s_sdl3 || s_offscreen ||
        sr_capture_is_armed()) return;
    int result = sdl3vk_present_held();
    sync_sdl_input();
    if (result == 0) gui_quit_request();
    /* Shown, or not: a display too busy to take it leaves the frame owed, tried again at the
     * next VBLANK or shown by the next flip (ace.h); a failed present or a closed window gives it
     * up, and it is counted. */
    if (result == 1) ace_smooth_note_present(ACE_PRESENT_HELD, SDL_GetTicksNS());
    else ace_smooth_held_missed(result == SDL3VK_PRESENT_DISPLAY_BUSY);
#else
    (void)vcount;
#endif
}
#endif /* SR_GUI_INPUT_PATH */

#ifndef SR_GUI_PRESENT_SELFTEST

int gui_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    if (!s_on) return 0;
    if (stride == 0) stride = 512;
    if (s_quit_requested) {
        /* After a quit no frame is presented: the window keeps the last one while the guest
         * runs its exit callback, until the scheduler stops. The window is still pumped. */
        gui_pump();
        return 0;
    }

    if (!present_slot_due()) {
        /* No presenter runs, so an armed capture stays unserviced; the caller resolves it
         * as "nothing attempted" when this returns (fbcap.h). */
        if (sr_perf_enabled) sr_perf_present_skip();
        gui_pump();
        return 0;
    }

    if (s_offscreen) {
        /* display_present_active validates the complete guest span before this
         * call. Conversion is the sink operation: the accepted frame is now
         * resident in the allocated host buffer, with no window or Vulkan WSI
         * dependency. */
        if (!display_cpu_source_current(fbaddr, fmt, stride)) {
            sr_capture_fail("guest framebuffer is unavailable for CPU presentation");
            return 0;
        }
        convert_fb(fbaddr, fmt, stride);
        s_offscreen_frames++;
        if (getenv("SR_PRESENT_TRACE")) {
            sr_gui_boot_event("BOOT_EVENT phase=frame_present backend=offscreen frame=%llu",
                    (unsigned long long)s_offscreen_frames);
        }
        /* The converted frame is what this sink accepted: an armed capture publishes
         * exactly these pixels, the same host frame the other presenters show. */
        sr_capture_serve_host(s_px, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX);
        sr_videorec_frame(s_px, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX);
        return 1;
    }

    int accepted = 1;
#ifdef SR_SDL3VK
    int gpu_action = GUI_GPU_TRY_CPU;   /* what the GPU present did */
    int cpu_result = GUI_CPU_NOT_RUN;   /* what the CPU present returned, when one ran */
    if (s_sdl3) {
        /* 1. GPU VRAM image fast path: any frame the GPU rasterizer produced is presented
         *    directly from its Vulkan image. GEGPU_PRESENT_SHOWN skips the CPU path;
         *    GEGPU_PRESENT_NOT_RESIDENT (CPU movie frames etc.) falls through to BGRA. */
        accepted = 0;
        /* While the scheduler paces the guest to real time (the default), a game frame never
         * waits for the display: a FIFO (VSync) swapchain that still holds every image returns
         * DISPLAY_BUSY instead of holding back the guest's VBLANK clock (gui_frame_may_skip_busy_display). */
        {
            extern int sched_vbl_paced(void);
            sdl3vk_set_present_nowait(gui_frame_may_skip_busy_display(sched_vbl_paced(), sr_capture_is_armed(),
                                                         sr_capture_pending_is_probe()));
        }
        /* While the on-screen keyboard is open the frame must pass through the host copy the
         * keyboard is drawn on, so the GPU fast path steps aside (the CPU path below). The
         * decision is taken once: osk_open also gates the guest-VRAM sync before that copy. */
        int osk_open = sr_osk_overlay_active();
        /* Ace Mode's crisp keyboard (ace.h): the GPU frame stays on and the UI pass draws the
         * keyboard over it at the window's resolution. A file or evidence capture takes the
         * CPU path for its frame, so the capture holds the keyboard; the picture probe changes
         * nothing. Off, or with the UI unavailable, this is today's path. */
        int osk_cpu = osk_open;
#ifndef SR_GUI_PRESENT_SELFTEST
        s_ace_keyboard_now = osk_open && s_ace_ui_ready && ace_effective()->text &&
                             !(sr_capture_is_armed() && !sr_capture_pending_is_probe());
        if (s_ace_keyboard_now) osk_cpu = 0;
#endif
#ifndef SR_GUI_PRESENT_SELFTEST
        gui_wide_update(fbaddr);
#endif
        int res = osk_cpu ? GEGPU_PRESENT_NOT_RESIDENT : gegpu_present(fbaddr, fmt, stride);
        sync_sdl_input();
        gpu_action = gui_gpu_result_action(res);
        if (gpu_action == GUI_GPU_CLOSED) gui_quit_request();
        /* Not shown, not failed: the CPU path must not show it either. */
        if (gpu_action == GUI_GPU_NOT_SHOWN && sr_perf_enabled) sr_perf_present_busy();
        if (gpu_action == GUI_GPU_SHOWN) {
            /* The GPU fast path never reaches convert_fb, which is the only emitter of
             * the first_frame boot milestone. Without this, boot_gate.py can never pass
             * on the default renderer even while the GPU presents every frame. There is
             * no host-side copy of these pixels to count, so the event reports its
             * source instead of a nonzero-pixel total; the gate accepts source=gpu as
             * proof a real frame reached the swapchain. */
            static int s_first_gpu_frame = 1;
            if (s_first_gpu_frame) {
                s_first_gpu_frame = 0;
                sr_gui_boot_event("BOOT_EVENT phase=first_frame source=gpu presented=1");
            }
            accepted = 1;
        }
        if (gpu_action != GUI_GPU_TRY_CPU) goto pace;

        /* 2. CPU-rendered fallback: convert guest VRAM to BGRA and blit through SDL3/Vulkan. */
        if (osk_cpu) {
            /* The GPU fast path was skipped for this frame, so a frame the rasterizer produced
             * may exist only in its Vulkan image. Materialize that target into guest VRAM before
             * convert_fb reads it, the boundary hle.c snapshot_sync_ok() enforces before a dump.
             * NO_TARGET: the frame was never GPU-resident and guest VRAM is already current.
             * FAILED: decline the frame rather than draw the keyboard over stale pixels. */
            GeGpuFbDescriptor d = { fbaddr, (uint32_t)fmt, stride, PSP_W, PSP_H };
            if (gegpu_sync_guest_fb(&d) == GEGPU_SYNC_FAILED) {
                static int s_osk_sync_warned = 0;
                if (s_osk_sync_warned < 3) {
                    s_osk_sync_warned++;
                    fprintf(stderr, "OSK: guest framebuffer synchronisation FAILED for addr=0x%08x "
                                    "fmt=%d stride=%u -- frame not presented\n", fbaddr, fmt, stride);
                }
                sr_capture_fail("keyboard guest framebuffer sync failed");
                goto pace;
            }
        }
        if (res == GEGPU_PRESENT_TARGET_FAILED) {
            /* gegpu_present() found a GPU-valid target for this frame, but preparing or presenting
             * it failed (issue #827). Its newest pixels may exist only in that Vulkan image, so
             * guest VRAM is stale. Materialize it with the same sync the keyboard path uses before
             * the fall-through below reads it. FAILED declines the frame instead of presenting the
             * older pixels still in guest VRAM. */
            GeGpuFbDescriptor d = { fbaddr, (uint32_t)fmt, stride, PSP_W, PSP_H };
            if (gegpu_sync_guest_fb(&d) == GEGPU_SYNC_FAILED) {
                static int s_target_sync_warned = 0;
                if (s_target_sync_warned < 3) {
                    s_target_sync_warned++;
                    fprintf(stderr, "GPU present: target preparation or presentation FAILED and guest "
                                    "framebuffer synchronisation FAILED for addr=0x%08x fmt=%d "
                                    "stride=%u -- frame not presented\n", fbaddr, fmt, stride);
                }
                sr_capture_fail("GPU present target failed: guest framebuffer sync failed");
                goto pace;
            }
        }
        if (!display_cpu_source_current(fbaddr, fmt, stride)) {
            sr_capture_fail("guest framebuffer is unavailable for CPU presentation");
            goto pace;
        }
        convert_fb(fbaddr, fmt, stride);
        /* The keyboard is drawn on the host copy only: the same pixels the window and the
         * frame capture receive, and guest VRAM is untouched. */
        if (osk_cpu && sr_osk_overlay_active())
            osk_overlay_paint(sr_osk_overlay_view(), s_px, PSP_W, PSP_H);
#ifndef SR_GUI_PRESENT_SELFTEST
        const int wide_cols = gui_wide_compose(fbaddr);
        if (wide_cols) {
            cpu_result = sdl3vk_present_rgba_ex(s_wide_px, wide_cols, PSP_H);
            /* Shown: the buffer's next draw begins a new frame and clears its canvas. */
            if (cpu_result == 1) ge_wide_shown(fbaddr);
        } else
#endif
        cpu_result = sdl3vk_present_rgba(s_px);
        sync_sdl_input();
        if (cpu_result == 0) gui_quit_request();
        if (cpu_result == SDL3VK_PRESENT_DISPLAY_BUSY && sr_perf_enabled) sr_perf_present_busy();
        accepted = cpu_result == 1;
        goto pace;
    }
#endif

#ifdef _WIN32
    /* GDI fallback is keyboard-only; controllers are handled by the SDL3 gamepad
     * subsystem (src/rt/gpu_sdl3vk) on the default Vulkan path. */
    gui_pump();
    s_buttons = read_keys();
    s_lx = 128; s_ly = 128;
    s_pad_present = 0;

    if (!display_cpu_source_current(fbaddr, fmt, stride)) {
        sr_capture_fail("guest framebuffer is unavailable for CPU presentation");
        return 0;
    }
    convert_fb(fbaddr, fmt, stride);
    if (!gdi_blit()) {
        sr_capture_fail("GDI present failed");
        return 0;
    }
    /* The DIB just blitted is the converted frame: an armed capture publishes it. */
    sr_capture_serve_host(s_px, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX);
    sr_videorec_frame(s_px, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX);
#else
    /* No GDI presenter on this host and the SDL3 presenter is not active: gui_init
     * reported that, so there is nothing to present and nothing to claim. */
    return 0;
#endif

#ifdef SR_SDL3VK
pace:
#endif
#if defined(SR_SDL3VK) && !defined(SR_GUI_PRESENT_SELFTEST)
    if (accepted && s_ace_ui_ready && !s_ace_first_frame_ns) s_ace_first_frame_ns = SDL_GetTicksNS();
#endif
#ifdef SR_SDL3VK
    gui_ace_presented(accepted, gui_present_busy(gpu_action, cpu_result));
    gui_render_fallback_presented(accepted);
#endif
    if (accepted) {
        s_shown_fb.addr = fbaddr;
        s_shown_fb.fmt = fmt;
        s_shown_fb.stride = stride;
        s_shown_fb.valid = 1;
    }
    gui_headless_acceptance_record_present(accepted);
    /* Pace to ~60 Hz so the intro/menus play at a watchable speed. Skipped when the scheduler
     * already paces vblanks to real time (the default): pacing twice on unaligned grids makes
     * frames miss their vblank and costs a whole extra period (30/20 fps quantization). */
    {
        extern int sched_vbl_paced(void);
        if (sched_vbl_paced()) { s_last_ns = SDL_GetTicksNS(); return accepted; }
    }
    uint64_t now_ns = SDL_GetTicksNS();
    const uint64_t period_ns = 1000000000u / 60u;
    if (now_ns - s_last_ns < period_ns)
        SDL_DelayPrecise(period_ns - (now_ns - s_last_ns));
    s_last_ns = SDL_GetTicksNS();
    return accepted;
}

/* ---- in-game pause presenter (#1159) -------------------------------------------------
 * The scheduler stops the game at a switch point and calls these from its pause loop
 * (SrPauseHost, sched.c). While paused the window shows the frame the guest last had
 * displayed under the overlay scrim, redrawn at the display rate, and only waits on events
 * while minimised. The overlay owns all input: the game reads a released pad, and a button
 * held when the overlay closes stays hidden from the game until it is let go (sdl3vk.h).
 * These repeat presents are not guest presents: they never reach the frame capture, the
 * headless acceptance count, the first-frame milestone or the guest frame rate. */
#ifdef SR_SDL3VK
static int          s_pause_open;
static int          s_pause_resume;      /* the overlay asked to resume */
static int          s_pause_dirty;       /* the overlay image must be drawn again */
static int          s_pause_base_dirty;  /* the frozen frame under the scrim must be composed again */
static int          s_pause_have_frame;  /* s_px holds the frozen game frame */
static int          s_pause_failure_reported;
static uint64_t     s_pause_next_ns;
static SDL_Surface  *s_pause_base;       /* the frozen frame under the scrim, at the window's size */
static SDL_Surface  *s_pause_surface;    /* the overlay image: the base with the pages over it */
static SDL_Renderer *s_pause_ren;        /* draws the pages into s_pause_surface */
static NkOverlayHost *s_pause_host;      /* the pages; NULL shows the base alone (no SDL3_ttf or fonts) */
static int          s_pause_host_tried;
static NkOverlayControls s_pause_controls;
/* The developer pages (#314), filled only in developer mode. */
static NkOverlayVram s_pause_vram;
static NkUiImage     s_pause_vram_image;
static uint32_t     *s_pause_vram_px;    /* the decoded window, GE_VRAM_VIEW_MAX_* words */
static NkOverlayProfile s_pause_profile;
static SrPerfLiveRow s_pause_profile_rows[SR_PERF_LIVE_ROWS];
static char          s_pause_profile_top[2][64];

int sr_savedata_pause_busy(void);   /* hle.c: a save is in progress, so quitting is refused */

static void gui_pause_free_canvas(void) {
    nk_overlay_host_unbind(s_pause_host);
    if (s_pause_ren) SDL_DestroyRenderer(s_pause_ren);
    if (s_pause_surface) SDL_DestroySurface(s_pause_surface);
    if (s_pause_base) SDL_DestroySurface(s_pause_base);
    s_pause_ren = NULL;
    s_pause_surface = NULL;
    s_pause_base = NULL;
}

/* The frame under the overlay is the displayed framebuffer as it is now. A framebuffer the
 * GPU rasterizer drew is brought back to guest memory first, the coherence step the keyboard
 * takes before reading it; nothing the guest observes changes. */
static int gui_pause_freeze(void) {
    if (!s_shown_fb.valid) return 0;
    GeGpuFbDescriptor d = { s_shown_fb.addr, (uint32_t)s_shown_fb.fmt, s_shown_fb.stride, PSP_W, PSP_H };
    if (gegpu_sync_guest_fb(&d) == GEGPU_SYNC_FAILED) return 0;
    if (!display_cpu_source_current(s_shown_fb.addr, s_shown_fb.fmt, s_shown_fb.stride)) return 0;
    convert_fb_pixels(s_shown_fb.addr, s_shown_fb.fmt, s_shown_fb.stride);
    return 1;
}

/* Composes the base at the swapchain's size: the frozen frame placed where a game frame
 * would be (with the scaling in force), then the scrim over all of it. */
static int gui_pause_compose_base(int w, int h, SDL_PixelFormat format) {
    if (!s_pause_base || s_pause_base->w != w || s_pause_base->h != h || s_pause_base->format != format) {
        if (s_pause_base) SDL_DestroySurface(s_pause_base);
        s_pause_base = SDL_CreateSurface(w, h, format);
        if (!s_pause_base) return 0;
    }
    SDL_Renderer *ren = SDL_CreateSoftwareRenderer(s_pause_base);
    if (!ren) return 0;
    int ok = 1;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (s_pause_have_frame) {
        SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC,
                                             PSP_W, PSP_H);
        if (!tex || !SDL_UpdateTexture(tex, NULL, s_px, PSP_W * 4)) {
            ok = 0;
        } else {
            Sdl3VkPresentPlan plan = sdl3vk_present_plan(sdl3vk_present_scaling(), PSP_W, PSP_H, w, h, INT32_MAX);
            if (plan.w > 0 && plan.h > 0) {
                SDL_FRect dst = { (float)plan.x, (float)plan.y, (float)plan.w, (float)plan.h };
                SDL_SetTextureScaleMode(tex, plan.nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
                SDL_RenderTexture(ren, tex, NULL, &dst);
            }
        }
        if (tex) SDL_DestroyTexture(tex);
    }
    const uint32_t scrim = nk_ui_theme_n1()->scrim;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, (Uint8)(scrim >> 24), (Uint8)(scrim >> 16),
                           (Uint8)(scrim >> 8), (Uint8)scrim);
    SDL_RenderFillRect(ren, NULL);
    SDL_RenderPresent(ren);
    SDL_DestroyRenderer(ren);
    return ok;
}

static int gui_pause_scaling_to_overlay(int mode) {
    switch (mode) {
    case SDL3VK_SCALING_INTEGER:  return NK_OVERLAY_SCALING_INTEGER;
    case SDL3VK_SCALING_BILINEAR: return NK_OVERLAY_SCALING_SMOOTH;
    default:                      return NK_OVERLAY_SCALING_SHARP;
    }
}

static int gui_pause_scaling_from_overlay(int scaling) {
    switch (scaling) {
    case NK_OVERLAY_SCALING_INTEGER: return SDL3VK_SCALING_INTEGER;
    case NK_OVERLAY_SCALING_SMOOTH:  return SDL3VK_SCALING_BILINEAR;
    default:                         return SDL3VK_SCALING_SHARP;
    }
}

/* The controls page describes the mapping in force: the session's input profile, its keys
 * and pad bindings, with the pad's buttons named for the pad that is there. */
static void gui_pause_describe_controls(void) {
    nk_overlay_describe_controls(&s_pause_controls, sdl3vk_get_input_profile(),
                                 nk_overlay_host_pad_family(s_pause_host));
}

_Static_assert((int)GE_VRAM_VIEW_DISPLAY == (int)NK_OVERLAY_VRAM_DISPLAY &&
                   (int)GE_VRAM_VIEW_DRAW == (int)NK_OVERLAY_VRAM_DRAW &&
                   (int)GE_VRAM_VIEW_DEPTH == (int)NK_OVERLAY_VRAM_DEPTH &&
                   (int)GE_VRAM_VIEW_TEXTURE == (int)NK_OVERLAY_VRAM_TEXTURE &&
                   (int)GE_VRAM_VIEW_SOURCES == (int)NK_OVERLAY_VRAM_SOURCE_COUNT &&
                   GE_VRAM_VIEW_DEPTH16 == NK_OVERLAY_VRAM_DEPTH16,
               "the GE's VRAM view sources are the overlay page's, index for index");
/* The surfaces the display and the GE state name while the game is paused, from the same
 * ge.c function vram-view-identity-selftest runs (ge_shared.h): the displayed framebuffer, the
 * draw target, the depth buffer and the bound texture. */
static void gui_pause_describe_vram(void) {
    NkOverlayVram *v = &s_pause_vram;
    GeVramSources sources;
    ge_vram_view_sources(s_shown_fb.addr, s_shown_fb.stride, (uint32_t)s_shown_fb.fmt,
                         s_shown_fb.valid, &sources);
    for (int i = 0; i < GE_VRAM_VIEW_SOURCES; i++) {
        const GeVramWindow *w = &sources.window[i];
        NkOverlayVramSurface s = { w->addr, w->stride, w->width, w->height, w->format };
        v->sources[i] = s;
        v->present[i] = sources.present[i] != 0;
    }
    v->texture_swizzled = sources.texture_swizzled != 0;
}

/* Decodes the viewer's window into the page's image through ge_vram_view_decode, the decode
 * vram-view-identity-selftest runs. Reads only: the surface decoder copies guest bytes as
 * stored, and the bound texture goes through the texture decoder with the GE state left as it
 * is (ge_shared.h). Alpha is shown opaque, as the PSP display shows a framebuffer. */
static void gui_pause_decode_vram(void) {
    const NkOverlayVramView *view = nk_overlay_host_vram_view(s_pause_host);
    const size_t capacity = (size_t)GE_VRAM_VIEW_MAX_WIDTH * GE_VRAM_VIEW_MAX_HEIGHT;
    GeVramWindow window;
    uint32_t width, height;
    int result;
    s_pause_vram.image = NULL;
    s_pause_vram.note = NULL;
    if (!view || !view->chosen) return;
    if (!s_pause_vram_px) s_pause_vram_px = (uint32_t *)malloc(capacity * sizeof(uint32_t));
    if (!s_pause_vram_px) {
        s_pause_vram.note = "Out of memory for the decoded window.";
        return;
    }
    window.addr = view->view.addr;
    window.stride = view->view.stride;
    window.width = view->view.width;
    window.height = view->view.height;
    window.format = view->view.format;
    result = ge_vram_view_decode(view->source == NK_OVERLAY_VRAM_TEXTURE, &window,
                                 s_pause_vram_px, capacity, &width, &height);
    if (result == GE_VRAM_VIEW_TEXTURE_REFUSED) {
        s_pause_vram.note = "The bound texture cannot be read now: it is outside guest memory, "
                            "larger than 512 pixels, or a GE capture is running.";
        return;
    }
    if (result != GE_VRAM_VIEW_SHOWN) {
        s_pause_vram.note = "This window cannot be read now.";
        return;
    }
    s_pause_vram_image.width = width;
    s_pause_vram_image.height = height;
    s_pause_vram_image.rgba_pm = (uint8_t *)s_pause_vram_px;
    s_pause_vram_image.serial = nk_ui_image_new_serial();
    s_pause_vram.image = &s_pause_vram_image;
}

/* The Profile page's rows and bars from the nakagawa-perf-v1 counters (perf.h). Opening the
 * page turns the counters on when nothing else had: they collect from then on. */
static void gui_pause_describe_profile(void) {
    NkOverlayProfile *p = &s_pause_profile;
    const SrPerfHistory *history = sr_perf_history();
    int was_on = sr_perf_enabled;
    unsigned rows;
    size_t bars;
    memset(p, 0, sizeof(*p));
    if (!was_on) sr_perf_enable_counters();
    rows = sr_perf_live_rows(sr_perf_history_at(history, 0), s_pause_profile_rows,
                             SR_PERF_LIVE_ROWS);
    for (unsigned i = 0; i < rows; i++) {
        p->rows[p->row_count].label = s_pause_profile_rows[i].label;
        p->rows[p->row_count].value = s_pause_profile_rows[i].value;
        p->row_count++;
    }
    for (unsigned direction = 0; rows && direction < 2u; direction++) {
        SrPerfTopPc top;
        if (!sr_perf_top_transitions(direction, &top, 1)) continue;
        snprintf(s_pause_profile_top[direction], sizeof(s_pause_profile_top[direction]),
                 "0x%08X x%llu %s", (unsigned)top.pc, (unsigned long long)top.count, top.reason);
        p->rows[p->row_count].label = direction ? "Back to AOT" : "To interpreter";
        p->rows[p->row_count].value = s_pause_profile_top[direction];
        p->row_count++;
    }
    bars = history->count < NK_OVERLAY_PROFILE_BARS ? history->count : NK_OVERLAY_PROFILE_BARS;
    for (size_t i = 0; i < bars; i++) {
        const SrPerfLiveInterval *iv = sr_perf_history_at(history, (unsigned)(bars - 1u - i));
        double wall = iv && iv->wall_ns ? (double)iv->wall_ns : 0.0;
        if (!wall) continue;
        p->bars[p->bar_count][NK_OVERLAY_PROFILE_CPU] = (float)((double)iv->cpu_ns / wall);
        p->bars[p->bar_count][NK_OVERLAY_PROFILE_GE_WAIT] = (float)((double)iv->ge_wait_ns / wall);
        p->bars[p->bar_count][NK_OVERLAY_PROFILE_PRESENT] = (float)((double)iv->present_ns / wall);
        p->bars[p->bar_count][NK_OVERLAY_PROFILE_IDLE] = (float)((double)iv->idle_ns / wall);
        p->bar_count++;
    }
    p->note = rows ? "The last closed second; the transition PCs cover the whole run. In "
                     "developer mode F3 shows these rows live."
          : was_on ? "Resume for a second: the counters close an interval each second."
                   : "The counters start now. Resume the game, then pause again.";
}

/* Carries out what the person chose on a page. */
static void gui_pause_act(NkOverlayAction action) {
    switch (action.kind) {
    case NK_OVERLAY_ACT_RESUME:
        s_pause_resume = 1;
        break;
    case NK_OVERLAY_ACT_QUIT:
        /* Confirmed, and refused by the page while a save is in progress: the same quit
         * request a window close makes. The game resumes when it has an exit callback to
         * run; otherwise the pause loop's stop check ends the run (gui_pause_quit). */
        fprintf(stderr, "SR_PAUSE: quit to library\n");
        fflush(stderr);
        gui_quit_request();
        if (!gui_stop_requested()) s_pause_resume = 1;
        break;
    case NK_OVERLAY_ACT_SET_SCALING:
        if (sdl3vk_set_present_scaling(gui_pause_scaling_from_overlay(action.value)))
            s_pause_base_dirty = 1;
        s_pause_dirty = 1;
        break;
    case NK_OVERLAY_ACT_SET_HUD:
        sdl3vk_hud_set_enabled(action.value != 0);
        s_pause_dirty = 1;
        break;
    case NK_OVERLAY_ACT_SET_READOUT:
        ace_set_readout(action.value != 0);
        s_pause_dirty = 1;
        break;
    case NK_OVERLAY_ACT_SET_FULLSCREEN:
    case NK_OVERLAY_ACT_SET_PACING:
        /* A fullscreen change resizes the window; the pause loop composes the frame again
         * on the pixel-size event that follows. */
        (void)pause_display_act(action, sdl3vk_set_fullscreen);
        s_pause_dirty = 1;
        break;
    case NK_OVERLAY_ACT_VRAM_VIEW:
        if (sdl3vk_developer_mode()) gui_pause_decode_vram();
        s_pause_dirty = 1;
        break;
    default:
        break;
    }
}

/* Draws the overlay image at the swapchain's size: the base, then the pages over it. */
static int gui_pause_compose(int w, int h, int rgba) {
    SDL_PixelFormat format = rgba ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_BGRA32;
    if (!s_pause_surface || s_pause_surface->w != w || s_pause_surface->h != h ||
        s_pause_surface->format != format) {
        gui_pause_free_canvas();
        s_pause_surface = SDL_CreateSurface(w, h, format);
        s_pause_ren = s_pause_surface ? SDL_CreateSoftwareRenderer(s_pause_surface) : NULL;
        if (!s_pause_ren) {
            gui_pause_free_canvas();
            return 0;
        }
        s_pause_base_dirty = 1;
    }
    if (s_pause_base_dirty) {
        if (!gui_pause_compose_base(w, h, format)) return 0;
        s_pause_base_dirty = 0;
    }
    SDL_SetSurfaceBlendMode(s_pause_base, SDL_BLENDMODE_NONE);
    if (!SDL_BlitSurface(s_pause_base, NULL, s_pause_surface, NULL)) return 0;
    if (s_pause_host) {
        NkOverlayModel model;
        NkOverlayAction action;
        char game_title[SR_WINDOW_TITLE_GAME_MAX + 1];
        float display_scale = 1.0f, pixels_per_point = 1.0f;
        (void)sdl3vk_window_scales(&display_scale, &pixels_per_point);
        gui_pause_describe_controls();
        memset(&model, 0, sizeof(model));
        /* The launcher's SR_GAME_TITLE, cleaned as the window caption cleans it. */
        (void)sr_window_title_sanitize(getenv("SR_GAME_TITLE"), game_title, sizeof(game_title));
        model.title_name = game_title;
        model.disc_id = getenv("SR_DISC_ID");
        model.scaling = gui_pause_scaling_to_overlay(sdl3vk_present_scaling());
        model.hud_on = sdl3vk_hud_is_enabled() != 0;
        /* Enhanced or Ace (ace.h): the menu says what it does; the readout is a live switch while
         * the in-game UI can draw it. Nothing in an authentic run. */
        static char ace_caption[200];
        ace_caption[0] = '\0';
        if (g_ace_on) {
            char words[160];
            if (ace_feature_words(words, sizeof(words)) > 0)
                snprintf(ace_caption, sizeof(ace_caption), "%s: %s", ace_tier_name(), words);
            else
                snprintf(ace_caption, sizeof(ace_caption), "%s", ace_tier_name());
        }
        model.ace_caption = ace_caption;
        model.ace_readout = s_ace_ui_ready ? (ace_effective()->readout ? 2 : 1) : 0;
        /* The display rows (#2002): a window that can change mode, and the launcher's two
         * Frame pacing choices. */
        model.fullscreen = pause_fullscreen_row(sdl3vk_fullscreen());
        model.pacing = pause_pacing_row(present_cap());
        model.saving = sr_savedata_pause_busy() != 0;
        model.controls = s_pause_controls.rows;
        model.control_count = s_pause_controls.count;
        /* The developer pages exist only in developer mode (#314); the model carries them
         * nothing otherwise, and the pages refuse to draw without the flag. */
        model.developer = sdl3vk_developer_mode() != 0;
        if (model.developer) {
            gui_pause_describe_vram();
            model.vram = &s_pause_vram;
            if (nk_overlay_host_screen(s_pause_host) == NK_OVERLAY_SCREEN_PROFILE) {
                gui_pause_describe_profile();
                model.profile = &s_pause_profile;
            }
        }
        if (!nk_overlay_host_draw(s_pause_host, s_pause_ren, display_scale, pixels_per_point,
                                  &model, &action))
            return 0;
        SDL_RenderPresent(s_pause_ren);
        gui_pause_act(action);
    }
    return 1;
}

/* Every input and window event while the overlay is open (sdl3vk_set_input_claim). Guide
 * closes it from any page. The pages take the rest: Esc and the pad's back button go back a
 * page (and resume from the menu), F1 and the pad's Start resume. Without the pages, Esc,
 * F1 and the pad's back button (east) close it. */
static void gui_pause_input(const void *event, void *user) {
    const SDL_Event *ev = (const SDL_Event *)event;
    (void)user;
    switch (ev->type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (ev->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
            if (ev->gbutton.down) s_pause_resume = 1;
            return;
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        s_pause_dirty = 1;
        s_pause_base_dirty = 1;
        s_pause_next_ns = 0;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        s_pause_next_ns = 0;
        break;
    default:
        break;
    }
    if (nk_overlay_host_is_open(s_pause_host)) {
        if (nk_overlay_host_event(s_pause_host, ev)) {
            s_pause_dirty = 1;
            s_pause_next_ns = 0;
        }
        return;
    }
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!ev->key.repeat && (ev->key.key == SDLK_ESCAPE || ev->key.key == SDLK_F1))
            s_pause_resume = 1;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (ev->gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
            s_pause_resume = 1;
        break;
    default:
        break;
    }
}
#endif

int gui_pause_presenter(void) {
#ifdef SR_SDL3VK
    return s_sdl3;
#else
    return 0;
#endif
}

int gui_pause_busy_reason(void) {
    if (sr_osk_overlay_active()) return SR_PAUSE_REFUSED_KEYBOARD;
#ifdef SR_SDL3VK
    if (s_sdl3 && sdl3vk_renderer_terminal()) return SR_PAUSE_REFUSED_STOPPING;
#endif
    return 0;
}

void gui_pause_enter(void) {
#ifdef SR_SDL3VK
    if (!s_sdl3 || s_pause_open) return;
    s_pause_open = 1;
    s_pause_resume = 0;
    s_pause_dirty = 1;
    s_pause_base_dirty = 1;
    s_pause_next_ns = 0;
    sdl3vk_set_input_claim(gui_pause_input, NULL);
    s_buttons = 0;
    s_lx = 128;
    s_ly = 128;
    s_pause_have_frame = gui_pause_freeze();
    /* A new pause starts the viewer afresh (nk_overlay_open): nothing decoded is kept. */
    s_pause_vram.image = NULL;
    s_pause_vram.note = NULL;
    /* The pages load once, on the first pause: SDL3_ttf and the fonts come from the player's
     * folder. Without them the pause shows the frozen frame alone, and says why once. */
    if (!s_pause_host && !s_pause_host_tried) {
        char error[256];
        s_pause_host_tried = 1;
        s_pause_host = nk_overlay_host_create(getenv("SR_UI_ROOT"), error, sizeof(error));
        if (!s_pause_host)
            fprintf(stderr, "SR_PAUSE: the overlay pages are unavailable (%s); the pause shows the "
                            "frozen frame only\n", error);
    }
    nk_overlay_host_open(s_pause_host, sched_pause_source() == SR_PAUSE_SRC_PAD_LOST
                                           ? NK_OVERLAY_OPEN_PAD_LOST : NK_OVERLAY_OPEN_USER);
#endif
}

#ifdef SR_SDL3VK
/* A window close while paused, or a presenter that reports the window gone: a quit request.
 * The game resumes when it has an exit callback to run (or a save to finish); otherwise the
 * pause continues for one more pass, where sched.c's pause loop sees the stop and ends the
 * run without resuming the guest. */
static SrPauseFrame gui_pause_quit(void) {
    gui_quit_request();
    return gui_stop_requested() ? SR_PAUSE_FRAME_CONTINUE : SR_PAUSE_FRAME_RESUME;
}
#endif

SrPauseFrame gui_pause_frame(void) {
#ifdef SR_SDL3VK
    if (!s_sdl3 || !s_pause_open) return SR_PAUSE_FRAME_RESUME;
    uint64_t now = SDL_GetTicksNS();
    uint32_t wait_ms = 100u;   /* minimised: wait on events, waking for the stop hook */
    if (!sdl3vk_window_minimized())
        wait_ms = s_pause_next_ns > now ? (uint32_t)((s_pause_next_ns - now + 999999u) / 1000000u) : 0u;
    if (wait_ms) sdl3vk_wait_events(wait_ms);
    if (!sdl3vk_poll()) return gui_pause_quit();
    if (s_pause_resume) return SR_PAUSE_FRAME_RESUME;
    now = SDL_GetTicksNS();
    if (sdl3vk_window_minimized() || now < s_pause_next_ns) return SR_PAUSE_FRAME_CONTINUE;
    uint64_t period = sdl3vk_display_period_ns();
    s_pause_next_ns = now + (period ? period : PSP_SCANOUT_PERIOD_NS);
    /* A held direction repeats, and a new page's second frame navigates by its layout. */
    if (nk_overlay_host_due(s_pause_host, (double)now / 1e9)) s_pause_dirty = 1;

    int w = 0, h = 0, rgba = 0;
    if (!sdl3vk_overlay_extent(&w, &h, &rgba)) return SR_PAUSE_FRAME_CONTINUE;
    const void *px = NULL;
    int pitch = 0;
    if (s_pause_dirty) {
        s_pause_dirty = 0;
        if (!gui_pause_compose(w, h, rgba)) {
            if (!s_pause_failure_reported) {
                s_pause_failure_reported = 1;
                fprintf(stderr, "SR_PAUSE: the overlay image (%dx%d) could not be drawn: %s\n",
                        w, h, SDL_GetError());
            }
            s_pause_dirty = 1;
            return SR_PAUSE_FRAME_CONTINUE;
        }
        if (s_pause_resume) return SR_PAUSE_FRAME_RESUME;
        px = s_pause_surface->pixels;
        pitch = s_pause_surface->pitch;
    }
    int presented = sdl3vk_present_overlay(px, pitch);
    if (presented == 0) return gui_pause_quit();
    if (presented == 2) {
        s_pause_dirty = 1;
        s_pause_base_dirty = 1;
        s_pause_next_ns = 0;
    } else if (presented < 0) {
        /* A renderer that can no longer present cannot show the overlay; the game goes on
         * and meets the same terminal renderer it would have met without a pause. */
        if (sdl3vk_renderer_terminal()) return SR_PAUSE_FRAME_RESUME;
        s_pause_dirty = 1;
    }
    return SR_PAUSE_FRAME_CONTINUE;
#else
    return SR_PAUSE_FRAME_RESUME;
#endif
}

void gui_pause_leave(void) {
#ifdef SR_SDL3VK
    if (!s_sdl3 || !s_pause_open) return;
    /* Events queued since the last frame are still the overlay's; the held state they leave
     * is what the release mask hides. A window close among them is still a quit request. */
    if (!sdl3vk_poll()) gui_quit_request();
    sdl3vk_set_input_claim(NULL, NULL);
    sdl3vk_overlay_release();
    nk_overlay_host_close(s_pause_host);
    gui_pause_free_canvas();
    s_pause_open = 0;
    s_pause_resume = 0;
    s_buttons = sdl3vk_buttons();
    sdl3vk_analog(&s_lx, &s_ly);
#endif
}

#if defined(SR_SDL3VK) || defined(_WIN32)
/* Releases the presenter in reverse of gui_init: the pause canvas, the GPU GE (its objects live
 * on the presenter's device), the SDL3/Vulkan presenter, then the GDI window and the frame copy.
 * Each step clears what it released, so a second call does nothing. */
static void gui_release_presenter(void) {
#ifdef SR_SDL3VK
    if (s_sdl3) {
        if (s_pause_open) sdl3vk_set_input_claim(NULL, NULL);
        s_pause_open = 0;
        gui_pause_free_canvas();
        s_pause_vram.image = NULL;
        free(s_pause_vram_px);
        s_pause_vram_px = NULL;
#ifndef SR_GUI_PRESENT_SELFTEST
        gui_upscale_release();
        gui_wide_release();
        gui_ace_smooth_release();
        gui_ace_ui_release();
#endif
        gegpu_shutdown();
        sdl3vk_shutdown();
        s_sdl3 = 0;
    }
#endif
#ifdef _WIN32
    if (s_hwnd) DestroyWindow(s_hwnd);
    s_hwnd = NULL;
#endif
    free(s_px);
    s_px = NULL;
    s_on = 0;
}
#endif
#endif /* !SR_GUI_PRESENT_SELFTEST */

#ifdef SR_GUI_PRESENT_SELFTEST
/* A gate, not a debug aid: keep the assertions under release CFLAGS. */
#undef NDEBUG
#include <assert.h>

/* Synthetic host clock for the slot rule: an arbitrary non-zero origin, PSP scanouts one
 * period apart, and a submission offset inside its scanout. */
#define SELFTEST_T0_NS 1000000000000ull
#define SELFTEST_MS_NS 1000000ull

static void selftest_reset_slots(const char *cap) {
    s_present_cap = present_cap_from_value(cap);
    s_present_next_ns = 0;
}

/* The pause overlay's Frame pacing row (#2002) changes the cap mid-run: No extra cap presents
 * every flip at once, and PSP timing again drops a second flip inside one scanout. A value the
 * row cannot send reads back as PSP timing, never as uncapped. */
static void selftest_pacing_changes_while_running(void) {
    uint64_t t = SELFTEST_T0_NS;
    selftest_reset_slots(NULL);
    assert(present_slot_due_at(t));
    assert(!present_slot_due_at(t + 2u * SELFTEST_MS_NS));
    present_set_cap(0);
    assert(present_cap() == 0);
    assert(present_slot_due_at(t + 3u * SELFTEST_MS_NS));
    assert(present_slot_due_at(t + 4u * SELFTEST_MS_NS));
    assert(present_slot_due_at(t + 5u * SELFTEST_MS_NS));
    present_set_cap(PRESENT_CAP_SCANOUT);
    assert(present_cap() == PRESENT_CAP_SCANOUT);
    assert(present_slot_due_at(t + 6u * SELFTEST_MS_NS));
    assert(!present_slot_due_at(t + 8u * SELFTEST_MS_NS));
    assert(present_slot_due_at(t + 6u * SELFTEST_MS_NS + PSP_SCANOUT_PERIOD_NS));
    present_set_cap(-7);
    assert(present_cap() == PRESENT_CAP_SCANOUT);
    present_set_cap(PRESENT_CAP_ENV_UNRESOLVED);
    assert(present_cap() == PRESENT_CAP_SCANOUT);
}

static int s_selftest_fullscreen_calls;
static int s_selftest_fullscreen_on;

static int selftest_set_fullscreen(int on) {
    s_selftest_fullscreen_calls++;
    s_selftest_fullscreen_on = on;
    return 1;
}

/* The pause overlay's display rows (#2002) as gui.c fills and applies them: the rows the page
 * gets from the window and the cap in force, what each choice changes, and that a choice reads
 * back as the row the person picked. */
static void selftest_pause_display_rows(void) {
    NkOverlayAction act;
    int pacing;

    assert(pause_fullscreen_row(-1) == 0);
    assert(pause_fullscreen_row(0) == 1);
    assert(pause_fullscreen_row(1) == 2);
    assert(pause_pacing_row(PRESENT_CAP_SCANOUT) == 1 + NK_OVERLAY_PACING_PSP);
    assert(pause_pacing_row(0) == 1 + NK_OVERLAY_PACING_UNCAPPED);
    assert(pause_pacing_row(30) == 0);
    assert(pause_pacing_row(60) == 0);

    s_selftest_fullscreen_calls = 0;
    act.kind = NK_OVERLAY_ACT_SET_FULLSCREEN;
    act.value = 1;
    assert(pause_display_act(act, selftest_set_fullscreen));
    assert(s_selftest_fullscreen_calls == 1 && s_selftest_fullscreen_on == 1);
    act.value = 0;
    assert(pause_display_act(act, selftest_set_fullscreen));
    assert(s_selftest_fullscreen_calls == 2 && s_selftest_fullscreen_on == 0);

    selftest_reset_slots(NULL);
    for (pacing = 0; pacing < NK_OVERLAY_PACING_COUNT; pacing++) {
        act.kind = NK_OVERLAY_ACT_SET_PACING;
        act.value = pacing;
        assert(pause_display_act(act, selftest_set_fullscreen));
        assert(pause_pacing_row(present_cap()) == 1 + pacing);
    }
    assert(present_cap() == 0);
    act.value = NK_OVERLAY_PACING_COUNT;
    assert(pause_display_act(act, selftest_set_fullscreen));
    assert(present_cap() == PRESENT_CAP_SCANOUT);
    act.value = NK_OVERLAY_PACING_UNCAPPED;
    assert(pause_display_act(act, selftest_set_fullscreen));
    act.value = -1;
    assert(pause_display_act(act, selftest_set_fullscreen));
    assert(present_cap() == PRESENT_CAP_SCANOUT);
    assert(s_selftest_fullscreen_calls == 2);

    act.kind = NK_OVERLAY_ACT_SET_HUD;
    act.value = 0;
    assert(!pause_display_act(act, selftest_set_fullscreen));
    act.kind = NK_OVERLAY_ACT_SET_SCALING;
    assert(!pause_display_act(act, selftest_set_fullscreen));
    assert(s_selftest_fullscreen_calls == 2);
    assert(present_cap() == PRESENT_CAP_SCANOUT);
}

/* Small deterministic generator (the PCG32 output step over an LCG state) for jitter
 * that is not a repeating pattern. */
static uint32_t selftest_next_random(uint64_t *state) {
    uint64_t old = *state;
    *state = old * 6364136223846793005ull + 1442695040888963407ull;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

/* One flip per PSP scanout reaching the host with scheduling jitter. The first flip sets
 * the slot grid's phase; it arrives 3 ms late, so most later flips come earlier in their
 * scanout than the grid. Each is the only flip in its scanout and must be presented. The
 * floor rule (present only once the slot has started) dropped every flip that came ahead
 * of the first one's phase. */
static void selftest_jittered_scanout_flips_are_all_presented(void) {
    static const int32_t pattern_ms[] = { 3, -2, 1, -3, 0, 2, -1, -3, 3, -2 };
    const uint64_t period = PSP_SCANOUT_PERIOD_NS;
    selftest_reset_slots(NULL);
    unsigned presented = 0;
    for (unsigned k = 0; k < 600u; k++) {
        int64_t jitter = (int64_t)pattern_ms[k % (sizeof pattern_ms / sizeof pattern_ms[0])] *
                         (int64_t)SELFTEST_MS_NS;
        uint64_t now = SELFTEST_T0_NS + (uint64_t)k * period + (uint64_t)(jitter + 4 * (int64_t)SELFTEST_MS_NS);
        presented += (unsigned)present_slot_due_at(now);
    }
    assert(presented == 600u);

    /* The same with pseudo-random jitter of up to a fifth of a period either way. */
    selftest_reset_slots(NULL);
    uint64_t rng = 0x2c9277b5u;
    presented = 0;
    for (unsigned k = 0; k < 3600u; k++) {
        uint64_t span = (period * 2u) / 5u;
        uint64_t offset = (uint64_t)selftest_next_random(&rng) % (span + 1u);
        uint64_t now = SELFTEST_T0_NS + (uint64_t)k * period + offset;
        presented += (unsigned)present_slot_due_at(now);
    }
    assert(presented == 3600u);
}

/* Two flips in every scanout, a tenth and six tenths of the way through it: the cap still
 * presents no more than one per period over the run. */
static void selftest_two_flips_per_scanout_present_once_per_scanout(void) {
    const uint64_t period = PSP_SCANOUT_PERIOD_NS;
    selftest_reset_slots(NULL);
    unsigned presented = 0;
    const unsigned scanouts = 600u;
    for (unsigned k = 0; k < scanouts; k++) {
        uint64_t base = SELFTEST_T0_NS + (uint64_t)k * period;
        presented += (unsigned)present_slot_due_at(base + period / 10u);
        presented += (unsigned)present_slot_due_at(base + (period * 6u) / 10u);
    }
    assert(presented >= scanouts && presented <= scanouts + 1u);
}

/* Several submissions within one millisecond take one slot between them. */
static void selftest_burst_presents_once(void) {
    selftest_reset_slots(NULL);
    unsigned presented = 0;
    for (unsigned i = 0; i < 5u; i++)
        presented += (unsigned)present_slot_due_at(SELFTEST_T0_NS + (uint64_t)i * 200000ull);
    assert(presented == 1u);
}

/* An explicit 30 Hz cap over one jittered flip per scanout presents every other flip. */
static void selftest_explicit_cap_halves_scanout_flips(void) {
    static const int32_t pattern_ms[] = { 3, -2, 1, -3, 0, 2, -1, -3, 3, -2 };
    const uint64_t period = PSP_SCANOUT_PERIOD_NS;
    selftest_reset_slots("30");
    unsigned presented = 0;
    for (unsigned k = 0; k < 600u; k++) {
        int64_t jitter = (int64_t)pattern_ms[k % (sizeof pattern_ms / sizeof pattern_ms[0])] *
                         (int64_t)SELFTEST_MS_NS;
        uint64_t now = SELFTEST_T0_NS + (uint64_t)k * period + (uint64_t)(jitter + 4 * (int64_t)SELFTEST_MS_NS);
        presented += (unsigned)present_slot_due_at(now);
    }
    assert(presented >= 299u && presented <= 301u);
}

#ifdef SR_SDL3VK
/* ---- the input and quit hand-off after Ace Mode's held present (gui_ace_vblank) ----------
 * The sdl3vk and runtime calls that path makes, stubbed. Each stub either does what the test
 * expects of it or fails the test: a call nobody expected is a finding, never ignored. The
 * presenter's input model is sdl3vk's: a button pressed and released between two reads is kept
 * as a pulse until the game's sample consumes it, and sdl3vk_buttons reports held | pulses. */
static void selftest_unexpected(const char *name) {
    fprintf(stderr, "gui-present-selftest: unexpected call to %s\n", name);
    fflush(stderr);
    abort();
}

int sr_perf_enabled = 0;
int sr_sched_on = 1;
uint64_t sr_perf_now_ns_impl(void) { selftest_unexpected("sr_perf_now_ns_impl"); return 0; }

static uint32_t s_stub_held;          /* buttons down now */
static uint32_t s_stub_pulse;         /* pressed and released since the last consume */
static uint32_t s_stub_tap;           /* what the next held present's event read records */
static int      s_stub_held_result = -2; /* -2: sdl3vk_present_held is not expected */
static unsigned s_stub_held_calls;
static unsigned s_stub_stop_due_calls;

uint32_t sdl3vk_buttons(void) { return s_stub_held | s_stub_pulse; }
void sdl3vk_consume_button_pulses(void) { s_stub_pulse = 0; }
void sdl3vk_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = 128; if (ly) *ly = 128; }
int sdl3vk_pad_present(void) { return 1; }
/* Each input read takes the stick dead zone from the session's input profile; NULL (no profile)
 * gives 0, which is what every test but the dead-zone one expects. */
static const NkInputProfile *s_stub_profile;
const NkInputProfile *sdl3vk_get_input_profile(void) { return s_stub_profile; }
void sdl3vk_set_text_claim(int on) { if (on) selftest_unexpected("sdl3vk_set_text_claim(1)"); }
int sdl3vk_take_key_events(Sdl3VkKeyEvent *out, int max) { (void)out; (void)max; return 0; }
int sdl3vk_take_pause_request(void) { return SDL3VK_PAUSE_REQUEST_NONE; }
int sr_osk_overlay_active(void) { return 0; }
void sr_osk_overlay_key(OskKey key) { (void)key; selftest_unexpected("sr_osk_overlay_key"); }
void sr_osk_overlay_type(uint32_t codepoint) { (void)codepoint; selftest_unexpected("sr_osk_overlay_type"); }
SrPauseVerdict sched_pause_request(SrPauseSource source) {
    (void)source;
    selftest_unexpected("sched_pause_request");
    return (SrPauseVerdict)0;
}
int sr_exit_request_stop_due(void) { s_stub_stop_due_calls++; return 0; }
int sr_capture_is_armed(void) { return 0; }

/* The held present reads the window's events, as every present does: the tap lands there. */
static int (*s_stub_presenter)(VkImage src);   /* when set, it makes the held present */

int sdl3vk_present_held(void) {
    if (s_stub_presenter) return s_stub_presenter(VK_NULL_HANDLE);
    if (s_stub_held_result == -2) selftest_unexpected("sdl3vk_present_held");
    s_stub_held_calls++;
    s_stub_pulse |= s_stub_tap;
    s_stub_tap = 0;
    return s_stub_held_result;
}

/* hle_route.c's VBLANK sample: read the buttons, then consume the pulses it has passed on. */
static uint32_t selftest_game_sample(void) {
    uint32_t buttons = gui_buttons();
    gui_consume_button_pulses();
    return buttons;
}

/* One smooth-motion run whose next VBLANK presents the held frame. */
static void selftest_hold_frame(int result) {
    s_on = 1;
    s_sdl3 = 1;
    s_offscreen = 0;
    s_ace_smooth = (AceBetween *)&s_stub_held;   /* any non-NULL handle: never dereferenced */
    g_ace_on = 1;
    ace_smooth_shown_between(1000u);
    s_stub_held_result = result;
}

/* A paced present may skip a busy display, except for a frame an armed file or evidence capture
 * waits for; an unpaced run always waits. A busy GPU present is not shown and never falls
 * through to the CPU path, which would redraw it from guest VRAM. */
static void selftest_display_busy_frames(void) {
    assert(gui_frame_may_skip_busy_display(1, 0, 0) == 1);
    assert(gui_frame_may_skip_busy_display(1, 1, 1) == 1);   /* the picture probe */
    assert(gui_frame_may_skip_busy_display(1, 1, 0) == 0);   /* SR_FBDUMP, SR_FBSNAP, a capture tool */
    assert(gui_frame_may_skip_busy_display(0, 0, 0) == 0);   /* deterministic clock or turbo */
    assert(gui_frame_may_skip_busy_display(0, 1, 1) == 0);
    assert(gui_gpu_result_action(GEGPU_PRESENT_DISPLAY_BUSY) == GUI_GPU_NOT_SHOWN);
    assert(gui_gpu_result_action(GEGPU_PRESENT_SHOWN) == GUI_GPU_SHOWN);
    assert(gui_gpu_result_action(GEGPU_PRESENT_WINDOW_CLOSED) == GUI_GPU_CLOSED);
    assert(gui_gpu_result_action(GEGPU_PRESENT_NOT_RESIDENT) == GUI_GPU_TRY_CPU);
    assert(gui_gpu_result_action(GEGPU_PRESENT_TARGET_FAILED) == GUI_GPU_TRY_CPU);
}

/* A press and release that the held present's event read records reach the game's sample in
 * the same VBLANK; the sample then consumes it, as on any other VBLANK. */
static void selftest_held_present_passes_a_tap_to_the_game(void) {
    const uint32_t cross = 0x4000u;
    s_quit_requested = 0;
    s_buttons = 0;
    s_stub_held = 0;
    s_stub_pulse = 0;
    selftest_hold_frame(1);
    AcePaceStats before, after;
    ace_pace_stats(&before);
    s_stub_tap = cross;
    unsigned calls = s_stub_held_calls;
    gui_ace_vblank(1u);
    assert(s_stub_held_calls == calls + 1u);
    assert((selftest_game_sample() & cross) != 0u);
    assert((selftest_game_sample() & cross) == 0u);
    ace_pace_stats(&after);
    assert(after.kind[ACE_PRESENT_HELD] == before.kind[ACE_PRESENT_HELD] + 1u);
    assert(!gui_quit_requested());
    s_stub_held_result = -2;
}

/* A held present that finds the window closed requests quit, as gui_present does, and is not
 * counted as a present. */
static void selftest_held_present_window_closed_requests_quit(void) {
    s_quit_requested = 0;
    s_stub_stop_due_calls = 0;
    selftest_hold_frame(0);
    AcePaceStats before, after;
    ace_pace_stats(&before);
    gui_ace_vblank(1u);
    assert(gui_quit_requested());
    assert(s_stub_stop_due_calls == 1u);
    ace_pace_stats(&after);
    assert(after.kind[ACE_PRESENT_HELD] == before.kind[ACE_PRESENT_HELD]);
    /* After a quit no held frame is presented. */
    s_stub_held_result = -2;
    selftest_hold_frame(-2);
    gui_ace_vblank(1u);
    s_quit_requested = 0;
    g_ace_on = 0;
}

/* The game's sample reads the host's stick dead zone with the stick: the larger of the active
 * input profile's X and Y deadzone_inner in centred counts (ace_stick_host_counts), 0 with no
 * profile. */
static void selftest_stick_dead_zone_from_profile(void) {
    static NkInputProfile prof;
    const int sdl3 = s_sdl3;
    s_sdl3 = 1;
    memset(&prof, 0, sizeof(prof));
    s_stub_profile = NULL;
    (void)selftest_game_sample();
    assert(gui_stick_dead_zone() == 0);
    prof.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner = 7849;   /* the default profile's */
    prof.axes[NK_PSP_AXIS_ANALOG_Y].deadzone_inner = 7849;
    s_stub_profile = &prof;
    (void)selftest_game_sample();
    assert(gui_stick_dead_zone() == 30);
    prof.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner = 0;
    prof.axes[NK_PSP_AXIS_ANALOG_Y].deadzone_inner = 25700;  /* the larger axis, for both */
    (void)selftest_game_sample();
    assert(gui_stick_dead_zone() == 100);
    s_stub_profile = NULL;
    (void)selftest_game_sample();
    assert(gui_stick_dead_zone() == 0);
    s_sdl3 = sdl3;
}

/* ---- a held frame the display could not take (SDL3VK_PRESENT_DISPLAY_BUSY) ---------------
 * A 30 FPS title with smooth motion, run through the production show pass (gui_ace_show_pass),
 * its present's outcome (gui_ace_show_done), gui_present's busy and Ace steps (gui_present_busy,
 * gui_ace_presented) and the VBLANK's held present (gui_ace_vblank). Stubbed are the renderer and
 * the presenter. The renderer is a fake AceBetween that keeps the two newest frames and measures
 * each in-between it makes. Images are names: real frame n is BUSY_IMG_FRAME + n, the in-between
 * that leads to it BUSY_IMG_BETWEEN + n, so the presenter can tell what reached the screen. The
 * presenter stands in for present_common: a busy display returns before the show pass, a present
 * may be made to fail after its submission (an out-of-date swapchain), and each present's slot
 * wait has seen only the submission SDL3VK_PRESENT_FRAMES back finished, the least the real one
 * guarantees. A failed GPU present is retried by the CPU path, as gegpu_present's TARGET_FAILED
 * is in gui_present. `reflip`: the title flips each frame twice, so the second flip's own present
 * is the held one. */
enum { BUSY_FRAMES = 240, BUSY_FB = 0x44000, BUSY_IMG_FRAME = 0x10000, BUSY_IMG_BETWEEN = 0x20000 };
#define BUSY_NO_FAIL 0xFFFFFFFFu
typedef struct {
    int real[BUSY_FRAMES];      /* times the frame's own picture reached the screen */
    int between[BUSY_FRAMES];   /* times its in-between did */
    int order_ok;               /* real pictures reached the screen in the game's order */
    int last_real;
    int (*busy_at)(uint32_t);   /* the display cannot take a present at that VBLANK */
    uint32_t vcount;            /* the VBLANK the presents are made in */
    uint32_t fail_vcount;       /* the first presents made in it fail after their submission */
    int fails;                  /* how many */
    uint64_t submitted;         /* present submissions made */
    unsigned keeps;             /* pictures the recording was told to keep (sr_videorec_frame_keep) */
} BusyRun;

static BusyRun *s_busy;

static struct {
    int have_new, have_old;     /* the frames the pass keeps, by number */
    int newest, older;
    uint64_t serial;            /* in-betweens made */
    uint64_t submission[ACE_SMOOTH_MOTION_RING];  /* the submission carrying each one's measurement */
    uint64_t completed;         /* what sdl3vk_present_completed reports */
    uint64_t lost;              /* the one measurement that cannot be read; 0 for none */
} s_stub_smooth;

static VkImage busy_image(int base, int n) { return (VkImage)(uintptr_t)(unsigned)(base + n); }

VkImage ace_smooth_held(AceBetween *s, VkCommandBuffer cmd) {
    (void)s; (void)cmd;
    return s_stub_smooth.have_new ? busy_image(BUSY_IMG_FRAME, s_stub_smooth.newest) : VK_NULL_HANDLE;
}

VkImage ace_smooth_previous(AceBetween *s, VkCommandBuffer cmd) {
    (void)s; (void)cmd;
    return s_stub_smooth.have_old ? busy_image(BUSY_IMG_FRAME, s_stub_smooth.older) : VK_NULL_HANDLE;
}

void ace_smooth_unrecord(AceBetween *s) {
    (void)s;
    if (!s_stub_smooth.have_old) return;
    s_stub_smooth.newest = s_stub_smooth.older;
    s_stub_smooth.have_old = 0;
}

/* A failed present is an out-of-date swapchain, unless a run makes the renderer terminal. */
static int s_stub_terminal;
int sdl3vk_renderer_terminal(void) { return s_stub_terminal; }

void ace_smooth_size(const AceBetween *s, int *w, int *h) {
    (void)s;
    if (w) *w = PSP_W;
    if (h) *h = PSP_H;
}

VkImage ace_smooth_record_aux(AceBetween *s, VkCommandBuffer cmd, VkImage src, int w, int h, int between,
                              const AceSmoothAux *aux) {
    (void)s; (void)cmd; (void)w; (void)h; (void)aux;
    const int had = s_stub_smooth.have_new;
    s_stub_smooth.older = s_stub_smooth.newest;
    s_stub_smooth.have_old = had;
    s_stub_smooth.newest = (int)(uintptr_t)src - BUSY_IMG_FRAME;
    s_stub_smooth.have_new = 1;
    if (!between || !had) return VK_NULL_HANDLE;
    s_stub_smooth.serial++;
    s_stub_smooth.submission[s_stub_smooth.serial % ACE_SMOOTH_MOTION_RING] = 0;   /* not submitted yet */
    return busy_image(BUSY_IMG_BETWEEN, s_stub_smooth.newest);
}

void ace_smooth_aux_stats(const AceBetween *s, AceSmoothAuxStats *out) { (void)s; (void)out; }
uint64_t ace_smooth_between_serial(const AceBetween *s) { (void)s; return s_stub_smooth.serial; }
const AceSmoothAux *ace_hud_take(VkImage src) { (void)src; return NULL; }
void ace_hud_stats(AceSmoothAuxStats *out) { (void)out; }
uint64_t sdl3vk_present_completed(void) { return s_stub_smooth.completed; }
void sr_videorec_frame_keep(void) { if (s_busy) s_busy->keeps++; }

/* A record read before the presenter saw its submission finish may be torn: that fails the test.
 * One the ring no longer holds is refused, as ace_smooth.c refuses it. */
int ace_smooth_motion(const AceBetween *s, uint64_t serial, AceSmoothMotion *out) {
    (void)s;
    const uint64_t submission = s_stub_smooth.submission[serial % ACE_SMOOTH_MOTION_RING];
    if (serial + ACE_SMOOTH_MOTION_RING <= s_stub_smooth.serial) return 0;
    if (!submission || submission > s_stub_smooth.completed)
        selftest_unexpected("ace_smooth_motion before its submission finished");
    if (serial == s_stub_smooth.lost) return 0;
    memset(out, 0, sizeof(*out));
    out->moving = 100u;
    out->sum_old = 300u;
    out->sum_new = 300u;
    return 1;
}

static void busy_run_shown(BusyRun *r, VkImage image) {
    const int name = (int)(uintptr_t)image;
    if (name >= BUSY_IMG_BETWEEN) {
        r->between[name - BUSY_IMG_BETWEEN]++;
        return;
    }
    const int frame = name - BUSY_IMG_FRAME;
    if (frame < r->last_real) r->order_ok = 0;
    r->last_real = frame;
    r->real[frame]++;
}

/* present_common, as far as the show pass sees it. */
static int busy_present(VkImage src) {
    BusyRun *r = s_busy;
    if (r->busy_at(r->vcount)) return SDL3VK_PRESENT_DISPLAY_BUSY;
    int result = 1;
    if (r->vcount == r->fail_vcount && r->fails > 0) {
        r->fails--;
        result = -1;
    }
    const uint64_t next = r->submitted + 1u;
    if (next > SDL3VK_PRESENT_FRAMES) s_stub_smooth.completed = next - SDL3VK_PRESENT_FRAMES;
    const uint64_t made = s_stub_smooth.serial;
    int w = PSP_W, h = PSP_H;
    VkImage alt = (VkImage)gui_ace_show_pass((void *)&s_stub_held, (void *)src, &w, &h);
    VkImage shown = alt ? alt : src;
    uint64_t submission = 0;
    if (!shown) {
        result = -1;                    /* nothing to show: nothing is submitted */
    } else {
        submission = ++r->submitted;
        if (s_stub_smooth.serial != made)
            s_stub_smooth.submission[s_stub_smooth.serial % ACE_SMOOTH_MOTION_RING] = submission;
        if (result == 1) busy_run_shown(r, shown);
    }
    gui_ace_show_done(result, submission);
    return result;
}

/* A flip's gui_present: the GPU present (gegpu_present turns a failed one into TARGET_FAILED),
 * the CPU present of the same frame when that one says to try it, then gui_present's own end. */
static void busy_flip(int frame) {
    const int gpu = busy_present(busy_image(BUSY_IMG_FRAME, frame));
    const int res = gpu == 1 ? GEGPU_PRESENT_SHOWN
                  : gpu == SDL3VK_PRESENT_DISPLAY_BUSY ? GEGPU_PRESENT_DISPLAY_BUSY : GEGPU_PRESENT_TARGET_FAILED;
    const int gpu_action = gui_gpu_result_action(res);
    int cpu_result = GUI_CPU_NOT_RUN;
    if (gpu_action == GUI_GPU_TRY_CPU) cpu_result = busy_present(busy_image(BUSY_IMG_FRAME, frame));
    gui_ace_presented(gpu_action == GUI_GPU_SHOWN || cpu_result == 1, gui_present_busy(gpu_action, cpu_result));
}

static void busy_run(BusyRun *r, int reflip, int (*busy_at)(uint32_t), uint32_t fail_vcount, int fails,
                     uint64_t lost) {
    memset(r, 0, sizeof(*r));
    r->order_ok = 1;
    r->last_real = -1;
    r->busy_at = busy_at;
    r->fail_vcount = fail_vcount;
    r->fails = fails;
    assert(ace_init_spec("v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=x2;sas=off;tess=off;twin=off", NULL));
    s_on = 1; s_sdl3 = 1; s_offscreen = 0; s_quit_requested = 0;
    s_ace_smooth = (AceBetween *)&s_stub_held;
    memset(&s_stub_smooth, 0, sizeof(s_stub_smooth));
    s_stub_smooth.lost = lost;
    ace_motion_queue_reset(&s_ace_motion_queue);
    s_ace_show_pending = ACE_SHOW_REAL;
    s_busy = r;
    s_stub_presenter = busy_present;
    uint64_t now = 1000000u;
    for (int n = 0; n < BUSY_FRAMES; n++) {
        const uint32_t v = 2u * (uint32_t)n;
        r->vcount = v;
        ace_note_target_draw(BUSY_FB);
        (void)ace_flip(BUSY_FB, 512u * 272u * 4u, v, now);
        busy_flip(n);
        gui_ace_vblank(v);
        now += PSP_SCANOUT_PERIOD_NS;
        r->vcount = v + 1u;
        if (reflip) {
            (void)ace_flip(BUSY_FB, 512u * 272u * 4u, v + 1u, now);
            busy_flip(n);
        }
        gui_ace_vblank(v + 1u);
        now += PSP_SCANOUT_PERIOD_NS;
    }
    s_stub_presenter = NULL;
    s_busy = NULL;
}

static int busy_never(uint32_t v) { (void)v; return 0; }
static int busy_one_held(uint32_t v) { return v == 401u; }          /* frame 200's held present */
static int busy_every_odd(uint32_t v) { return v >= 301u && (v & 1u); }
static int busy_stretch(uint32_t v) { return v >= 401u && v <= 404u; }

static int busy_count(const int *a, int from, int to) {
    int n = 0;
    for (int i = from; i < to; i++) n += a[i] != 0;
    return n;
}

static int busy_most(const int *a) {
    int most = 0;
    for (int i = 0; i < BUSY_FRAMES; i++) if (a[i] > most) most = a[i];
    return most;
}

static int busy_sum(const int *a) {
    int n = 0;
    for (int i = 0; i < BUSY_FRAMES; i++) n += a[i];
    return n;
}

/* Every in-between that reached the screen is measured, or counted unread, or still waits for
 * its submission; none is measured twice, and nothing else is. The recording kept each one. */
static void busy_check_motion(const BusyRun *r, uint64_t unread) {
    AceMotionStats m;
    ace_motion_stats(&m);
    const uint64_t shown = (uint64_t)busy_sum(r->between);
    assert(m.unread == unread);
    assert(m.measured + m.unread + s_ace_motion_queue.count == shown);
    assert(s_ace_motion_queue.dropped == 0u);
    assert(r->keeps == (unsigned)shown);
}

static void busy_check_report(const char *want) {
    char json[ACE_REPORT_MAX];
    assert(ace_report_json(json, sizeof(json)) > 0);
    if (!strstr(json, want)) {
        fprintf(stderr, "gui-present-selftest: the report lacks %s. The report:\n%s", want, json);
        abort();
    }
}

static void selftest_busy_display_keeps_real_frames(void) {
    BusyRun r;
    AcePaceStats st;
    static const uint64_t busy_one[2] = { 1u, 2u };     /* reflip: its flip's present and its VBLANK */
    static const uint64_t busy_odd[2] = { 90u, 180u };
    static const uint64_t busy_four[2] = { 4u, 5u };
    for (int reflip = 0; reflip <= 1; reflip++) {
        /* The display takes every present: each frame has its in-between and its own picture. */
        busy_run(&r, reflip, busy_never, BUSY_NO_FAIL, 0, 0u);
        ace_pace_stats(&st);
        assert(busy_count(r.real, 0, BUSY_FRAMES) == BUSY_FRAMES && r.order_ok);
        assert(busy_count(r.between, 100, BUSY_FRAMES) == BUSY_FRAMES - 100);
        assert(st.busy == 0u && st.superseded == 0u);
        busy_check_motion(&r, 0u);
        if (!reflip) assert(busy_most(r.real) == 1 && busy_most(r.between) == 1);

        /* One held present finds the display busy: that frame is shown by the next flip's
         * present in place of the in-between, the new frame at its own VBLANK, and the frames
         * after carry on with in-betweens. */
        busy_run(&r, reflip, busy_one_held, BUSY_NO_FAIL, 0, 0u);
        ace_pace_stats(&st);
        assert(r.between[200] && r.real[200]);              /* was: never shown as itself */
        assert(!r.between[201] && r.real[201]);
        assert(r.between[202] && r.real[202]);
        assert(busy_count(r.real, 0, BUSY_FRAMES) == BUSY_FRAMES && r.order_ok);
        assert(st.busy == busy_one[reflip] && st.superseded == 0u);
        busy_check_motion(&r, 0u);
        busy_check_report(reflip ? "\"superseded\": 0, \"unpaired\": 0, \"busy\": 2}"
                                 : "\"superseded\": 0, \"unpaired\": 0, \"busy\": 1}");

        /* A display that takes only every second present, the flips': every game frame is
         * shown as itself, in order, and no made-up picture takes a real one's place. */
        busy_run(&r, reflip, busy_every_odd, BUSY_NO_FAIL, 0, 0u);
        ace_pace_stats(&st);
        assert(busy_count(r.real, 0, BUSY_FRAMES - 1) == BUSY_FRAMES - 1 && r.order_ok);
        assert(busy_count(r.between, 152, BUSY_FRAMES) == 0);
        assert(st.busy == busy_odd[reflip] && st.superseded == 0u);
        busy_check_motion(&r, 0u);

        /* The display is busy for four VBLANKs: frame 200's held present, frame 201's flip and
         * VBLANK, frame 202's flip. Frames 201 and 202 never reach a show pass, as without smooth
         * motion. At frame 202's VBLANK the display takes a present again, and the owed frame
         * 200 is shown there, never replaced by a newer one. A title that flips each frame twice
         * re-flips 202 there, so 202's picture was taken too: that present shows 200 and holds
         * 202, which the next flip shows in place of 203's in-between. Either way every frame
         * whose present was taken is shown, in order, nothing is superseded, no in-between
         * stands in for a real frame, and in-betweens resume with frame 204. */
        busy_run(&r, reflip, busy_stretch, BUSY_NO_FAIL, 0, 0u);
        ace_pace_stats(&st);
        assert(r.order_ok && r.real[200] == 1 && !r.real[201] && !r.between[201] && !r.between[202]);
        assert(reflip ? r.real[202] == 1 : !r.real[202]);
        assert(r.real[203] && !r.between[203] && r.between[204] && r.real[204]);
        assert(busy_most(r.real) == 1 + reflip);    /* a re-flip's present shows its frame again */
        assert(st.busy == busy_four[reflip] && st.superseded == 0u);
        busy_check_motion(&r, 0u);
    }

    /* The present that should show the owed frame 200 fails after its submission (frame 201's
     * flip). Nothing reached the screen, so 200 stays owed, and gui_present's CPU present of the
     * same flip shows it and holds 201, which its VBLANK shows. Every frame is shown once, in
     * order; the failed present is counted with the busy ones. */
    busy_run(&r, 0, busy_one_held, 402u, 1, 0u);
    ace_pace_stats(&st);
    assert(r.real[200] == 1 && r.between[200] && r.real[201] == 1 && !r.between[201]);
    assert(r.real[202] == 1 && r.between[202] == 1 && r.order_ok && busy_most(r.real) == 1);
    assert(busy_count(r.real, 0, BUSY_FRAMES) == BUSY_FRAMES);
    assert(st.busy == 2u && st.superseded == 0u);
    busy_check_motion(&r, 0u);
    busy_check_report("\"superseded\": 0, \"unpaired\": 0, \"busy\": 2}");

    /* Both presents of frame 201's flip fail, the GPU one and the CPU one: 200 stays owed and its
     * VBLANK shows it. 201 never reached the screen, as a frame whose presents fail is lost
     * without smooth motion, and 202 follows 200 as itself, since no in-between pairs it with a
     * frame two steps back. */
    busy_run(&r, 0, busy_one_held, 402u, 2, 0u);
    ace_pace_stats(&st);
    assert(r.real[200] == 1 && r.between[200] && !r.real[201] && !r.between[201]);
    assert(r.real[202] == 1 && !r.between[202] && r.between[203] && r.order_ok && busy_most(r.real) == 1);
    assert(busy_count(r.real, 0, BUSY_FRAMES) == BUSY_FRAMES - 1);
    assert(st.busy == 3u && st.superseded == 0u);
    busy_check_motion(&r, 0u);
    busy_check_report("\"superseded\": 0, \"unpaired\": 1, \"busy\": 3}");

    /* The same failed present with a renderer that can present nothing any more: nothing can
     * show frame 200 later, so it is given up and counted, and 201 is not held, so no VBLANK
     * shows it a second time. Only the give-up's accounting is checked here: a real terminal
     * renderer fails the CPU present too, which this stand-in still lets through. */
    s_stub_terminal = 1;
    busy_run(&r, 0, busy_one_held, 402u, 1, 0u);
    s_stub_terminal = 0;
    ace_pace_stats(&st);
    assert(!r.real[200] && r.between[200] && r.real[201] == 1 && !r.between[201]);
    assert(r.real[202] == 1 && r.between[202] == 1 && r.order_ok && busy_most(r.real) == 1);
    assert(busy_count(r.real, 0, BUSY_FRAMES) == BUSY_FRAMES - 1);
    assert(st.busy == 1u && st.superseded == 1u);
    busy_check_motion(&r, 0u);
    busy_check_report("\"superseded\": 1, \"unpaired\": 0, \"busy\": 1}");

    /* An in-between's present fails after its submission (frame 150's flip): nobody saw it, so it
     * is not measured, and its frame, shown by the CPU path, is not held for a second showing. */
    busy_run(&r, 0, busy_never, 300u, 1, 0u);
    ace_pace_stats(&st);
    assert(!r.between[150] && r.real[150] == 1 && r.between[151] == 1 && busy_most(r.real) == 1);
    assert(busy_count(r.real, 0, BUSY_FRAMES) == BUSY_FRAMES && r.order_ok);
    assert(st.busy == 0u && st.superseded == 0u);
    busy_check_motion(&r, 0u);

    /* A measurement that cannot be read when its submission has finished is counted, never
     * guessed. */
    busy_run(&r, 0, busy_never, BUSY_NO_FAIL, 0, 5u);
    busy_check_motion(&r, 1u);
    g_ace_on = 0;
}

/* ---- the render fallback rule and its event (#2151) -----------------------------------------
 * gui_present's step after each present (gui_render_fallback_presented) with the GE's counts
 * stubbed: the boot event it writes goes to SR_BOOT_EVENT_FILE, as in a player launch, and is
 * read back with the player's parser. Each case uses the rule's own constants, so a change to
 * them is tested as it stands. */
static GeGpuFallbackCounts s_stub_fallback;
void gegpu_fallback_counts(GeGpuFallbackCounts *out) { *out = s_stub_fallback; }

static char s_fallback_events_path[1024];

static void fallback_reset(void) {
    FILE *f = fopen(s_fallback_events_path, "wb");
    assert(f);
    fclose(f);
    memset(&s_render_window, 0, sizeof(s_render_window));
    memset(&s_stub_fallback, 0, sizeof(s_stub_fallback));
    s_stub_fallback.ready = 1;
    s_stub_fallback.gpu_prims = 0xFFFFFF00u;   /* the GPU count wraps during each case */
    gui_render_fallback_presented(1);          /* the first sample only primes the window */
}

/* One window of accepted presents; the first carries all of the window's counts, and a present
 * the slot rule dropped (accepted 0) comes between each two. */
static void fallback_window(int r, uint32_t gpu, unsigned long long sw, unsigned long long rb) {
    for (uint32_t f = 0; f < NK_RENDER_FALLBACK_WINDOW_FRAMES; f++) {
        if (f == 0) {
            s_stub_fallback.gpu_prims += gpu;
            s_stub_fallback.sw_prims[r] += sw;
            s_stub_fallback.readbacks[r] += rb;
        }
        gui_render_fallback_presented(1);
        gui_render_fallback_presented(0);
    }
}

/* The events written since fallback_reset; every line must be one. */
static int fallback_events(NkRenderFallbackEvent *out, int max) {
    FILE *f = fopen(s_fallback_events_path, "rb");
    char line[512];
    int n = 0;
    assert(f);
    while (fgets(line, sizeof(line), f)) {
        assert(n < max);
        if (!nk_render_fallback_event_parse(line, &out[n])) {
            fprintf(stderr, "gui-present-selftest: not a render_fallback event: %s", line);
            abort();
        }
        n++;
    }
    fclose(f);
    return n;
}

static void selftest_render_fallback_rule(int argc, char **argv) {
    NkRenderFallbackEvent ev[8];
    const uint32_t W = NK_RENDER_FALLBACK_WINDOW_FRAMES, K = NK_RENDER_FALLBACK_WINDOWS;
    const unsigned long long P = NK_RENDER_FALLBACK_MIN_PRIMS, B = NK_RENDER_FALLBACK_READBACKS;
    const uint32_t G = 400000u;                  /* GPU primitives a window: a busy scene */
    assert(K >= 2u && P >= 2u && B >= 1u);
    assert(argc >= 1 && argv[0]);
    (void)snprintf(s_fallback_events_path, sizeof(s_fallback_events_path), "%s.events", argv[0]);
#ifdef _WIN32
    assert(_putenv_s("SR_BOOT_EVENT_FILE", s_fallback_events_path) == 0);
#else
    assert(setenv("SR_BOOT_EVENT_FILE", s_fallback_events_path, 1) == 0);
#endif
    s_render_stats = 0;

    /* The event's names are the GE's reasons. */
    for (int r = 0; r < GEGPU_FALLBACK_REASONS; r++)
        assert(strcmp(gegpu_fallback_token(r), nk_render_effect_name((NkRenderEffect)r)) == 0);

    /* At the primitive threshold: nothing until the K-th window in a row, then one event that
     * carries those windows' counts; never a second one in the same run. */
    fallback_reset();
    for (uint32_t i = 0; i + 1u < K; i++) fallback_window(GEGPU_FALLBACK_STENCIL, G, P, 0u);
    assert(fallback_events(ev, 8) == 0);
    fallback_window(GEGPU_FALLBACK_STENCIL, G, P, 0u);
    assert(fallback_events(ev, 8) == 1);
    assert(ev[0].reason == NK_RENDER_EFFECT_STENCIL && ev[0].frames == K * W &&
           ev[0].sw_prims == K * P && ev[0].gpu_prims == (unsigned long long)K * G &&
           ev[0].readbacks == 0u && ev[0].share_pm == (uint32_t)(1000u * P / (G + P)));
    for (uint32_t i = 0; i < K + 1u; i++) fallback_window(GEGPU_FALLBACK_STENCIL, G, P, 0u);
    assert(fallback_events(ev, 8) == 1);

    /* One primitive short at any share (all of them here), or one readback short: never. */
    fallback_reset();
    for (uint32_t i = 0; i < 2u * K; i++) fallback_window(GEGPU_FALLBACK_LINE, 0u, P - 1u, 0u);
    for (uint32_t i = 0; i < 2u * K; i++)
        fallback_window(GEGPU_FALLBACK_RAM_FB, 1000000u, 1u, B - 1u);
    assert(fallback_events(ev, 8) == 0);

    /* The cost decides, not the share: a busy scene of 10,000 GPU primitives a present with 90 a
     * present drawn in software and no readback is under 1 % of the scene but about 2 ms a
     * present at the derived cost, and is reported with its share. */
    fallback_reset();
    for (uint32_t i = 0; i < K; i++)
        fallback_window(GEGPU_FALLBACK_RAM_FB, 10000u * W, 90u * W, 0u);
    assert(fallback_events(ev, 8) == 1 && ev[0].reason == NK_RENDER_EFFECT_RAM_FB &&
           ev[0].sw_prims == 90ull * W * K && ev[0].readbacks == 0u && ev[0].share_pm == 8u);

    /* A window under the rule breaks the run of windows: K-1 over, one under, K-1 over is not
     * enough; one more over is. */
    fallback_reset();
    for (uint32_t i = 0; i + 1u < K; i++) fallback_window(GEGPU_FALLBACK_COLOR_TEST, G, P, 0u);
    fallback_window(GEGPU_FALLBACK_COLOR_TEST, G, P - 1u, 0u);
    for (uint32_t i = 0; i + 1u < K; i++) fallback_window(GEGPU_FALLBACK_COLOR_TEST, G, P, 0u);
    assert(fallback_events(ev, 8) == 0);
    fallback_window(GEGPU_FALLBACK_COLOR_TEST, G, P, 0u);
    assert(fallback_events(ev, 8) == 1 && ev[0].reason == NK_RENDER_EFFECT_COLOR_TEST);

    /* Readbacks alone cross the rule, at a negligible share of primitives. */
    fallback_reset();
    for (uint32_t i = 0; i < K; i++)
        fallback_window(GEGPU_FALLBACK_WRITE_MASK, 1000000u, 1u, B);
    assert(fallback_events(ev, 8) == 1 && ev[0].reason == NK_RENDER_EFFECT_WRITE_MASK &&
           ev[0].readbacks == (unsigned long long)K * B && ev[0].sw_prims == K &&
           ev[0].share_pm == 0u);

    /* Two reasons in the same windows, half of all primitives each: each is reported, in reason
     * order. */
    fallback_reset();
    for (uint32_t i = 0; i < K; i++) {
        s_stub_fallback.sw_prims[GEGPU_FALLBACK_POINT] += P;
        fallback_window(GEGPU_FALLBACK_LOGIC_OP, 0u, P, 0u);
    }
    assert(fallback_events(ev, 8) == 2 && ev[0].reason == NK_RENDER_EFFECT_LOGIC_OP &&
           ev[1].reason == NK_RENDER_EFFECT_POINT && ev[0].frames == K * W &&
           ev[0].share_pm == 500u && ev[1].share_pm == 500u);

    /* The GPU GE stopped (ready 0): nothing is counted, and its counts starting over make a new
     * window rather than a negative one. */
    fallback_reset();
    for (uint32_t i = 0; i + 1u < K; i++) fallback_window(GEGPU_FALLBACK_MIPS, G, P, 0u);
    s_stub_fallback.ready = 0;
    gui_render_fallback_presented(1);
    memset(&s_stub_fallback, 0, sizeof(s_stub_fallback));
    s_stub_fallback.ready = 1;
    gui_render_fallback_presented(1);           /* primes again */
    fallback_window(GEGPU_FALLBACK_MIPS, G, P, 0u);
    assert(fallback_events(ev, 8) == 0);
    for (uint32_t i = 0; i + 1u < K; i++) fallback_window(GEGPU_FALLBACK_MIPS, G, P, 0u);
    assert(fallback_events(ev, 8) == 1 && ev[0].reason == NK_RENDER_EFFECT_MIPS);

    remove(s_fallback_events_path);
}
#endif /* SR_SDL3VK */

/* The unset SR_FPS_CAP default is the PSP scanout period, not a slower title-specific
 * rate: a guest may submit a framebuffer several times per scanout and every submission
 * past the next output slot is dropped rather than delaying the guest. */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s_present_cap = present_cap_from_value(NULL);
    assert(PSP_SCANOUT_PERIOD_NS == 16683333ull);
    assert(present_period_ns() == PSP_SCANOUT_PERIOD_NS);
    assert(present_period_ns() != 1000000000ull / 30u);
    assert(present_cap_from_value("native") == PRESENT_CAP_SCANOUT);
    assert(present_cap_from_value("-1") == PRESENT_CAP_SCANOUT);
    assert(present_cap_from_value("invalid") == PRESENT_CAP_SCANOUT);
    /* Through the slot logic: the first submission takes the slot, a second one in the
     * same scanout is dropped. */
    assert(present_slot_due());
    assert(!present_slot_due());

    s_present_cap = present_cap_from_value("0");
    assert(present_slot_due());

    s_present_cap = present_cap_from_value("30");
    assert(present_period_ns() == 1000000000ull / 30u);

    selftest_pacing_changes_while_running();
    selftest_pause_display_rows();

    selftest_jittered_scanout_flips_are_all_presented();
    selftest_two_flips_per_scanout_present_once_per_scanout();
    selftest_burst_presents_once();
    selftest_explicit_cap_halves_scanout_flips();
#ifdef SR_SDL3VK
    selftest_display_busy_frames();
    selftest_held_present_passes_a_tap_to_the_game();
    selftest_held_present_window_closed_requests_quit();
    selftest_stick_dead_zone_from_profile();
    selftest_busy_display_keeps_real_frames();
    selftest_render_fallback_rule(argc, argv);
#endif
    return 0;
}
#endif
