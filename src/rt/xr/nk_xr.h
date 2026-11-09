// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// nk_xr: the OpenXR (VR) module. OpenXR is loaded at run time and never linked:
// the only symbol taken from the loader library is xrGetInstanceProcAddr, and every
// other call goes through the function table below. Without a loader, a runtime, the
// required extension or a headset, the module refuses with a named reason and the
// caller carries on in the flat-screen path.
//
// The module creates no threads. The caller owns the XR frame thread and drives
// nk_xr_session_next(), nk_xr_do_action() and nk_xr_frame_once() from it. The frame
// loop never calls into the producer of the mailbox and the producer never waits on
// the frame loop.

#ifndef NK_XR_H
#define NK_XR_H

// The module calls OpenXR only through function pointers; declare no linkable xr*
// functions (see src/rt/xr/openxr/PROVENANCE.md).
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif

#include <stdint.h>

#include "xr/nk_xr_mailbox.h"
#include "xr/openxr/openxr.h"

#ifdef __cplusplus
extern "C" {
#endif

// The extension the module requires. Its name macro lives in openxr_platform.h behind the
// graphics-API guards, so the module names it itself.
#define NK_XR_EXT_VULKAN_ENABLE2 "XR_KHR_vulkan_enable2"

#ifdef _WIN32
#define NK_XR_LOADER_NAME "openxr_loader.dll"
#else
#define NK_XR_LOADER_NAME "libopenxr_loader.so.1"
#endif

// Why VR was refused. NK_XR_OK means no refusal.
typedef enum NkXrRefusal {
    NK_XR_OK = 0,
    NK_XR_REFUSE_NO_RUNTIME,         // loader library missing, or no active OpenXR runtime
    NK_XR_REFUSE_NO_VULKAN_ENABLE2,  // runtime lacks XR_KHR_vulkan_enable2
    NK_XR_REFUSE_INSTANCE,           // instance creation (or resolving its functions) failed
    NK_XR_REFUSE_NO_HMD,             // the runtime reports no head-mounted display
    NK_XR_REFUSE_OUT_OF_MEMORY       // host allocation failed
} NkXrRefusal;

// The text of a refusal: "ok", "no OpenXR runtime", "runtime lacks
// XR_KHR_vulkan_enable2", "instance creation failed", "no HMD", "out of memory".
// Never NULL; a value outside the enum gives "unknown refusal".
const char *nk_xr_refusal_name(NkXrRefusal r);

// The OpenXR functions the module uses, resolved from xrGetInstanceProcAddr.
typedef struct NkXrFns {
    void *lib;  // loader library handle; NULL when the table came from a proc address
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr;
    // Global functions (resolved with XR_NULL_HANDLE).
    PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_xrCreateInstance CreateInstance;
    // Instance functions (resolved by nk_xr_load_instance_fns()).
    PFN_xrDestroyInstance DestroyInstance;
    PFN_xrGetSystem GetSystem;
    PFN_xrPollEvent PollEvent;
    PFN_xrCreateSession CreateSession;
    PFN_xrBeginSession BeginSession;
    PFN_xrEndSession EndSession;
    PFN_xrDestroySession DestroySession;
    PFN_xrRequestExitSession RequestExitSession;
    PFN_xrWaitFrame WaitFrame;
    PFN_xrBeginFrame BeginFrame;
    PFN_xrEndFrame EndFrame;
    PFN_xrEnumerateEnvironmentBlendModes EnumerateEnvironmentBlendModes;
    // Set by nk_xr_probe(): the API version the runtime granted (XR_CURRENT_API_VERSION,
    // or XR_MAKE_VERSION(1, 0, 0) after a runtime refused 1.1) and the blend mode the
    // frame loop submits (OPAQUE when offered, else the runtime's first mode). Zero
    // before a probe; nk_xr_frame_once() then submits OPAQUE.
    XrVersion api_version;
    XrEnvironmentBlendMode blend_mode;
} NkXrFns;

// Open the loader library (`libname`, or NK_XR_LOADER_NAME when NULL) and resolve
// the global functions. Zeroes `*f` first. Returns NK_XR_OK, or
// NK_XR_REFUSE_NO_RUNTIME when the library cannot be opened or lacks
// xrGetInstanceProcAddr or a global function.
NkXrRefusal nk_xr_load(NkXrFns *f, const char *libname);

// The same from an already known xrGetInstanceProcAddr (a test injects a fake
// runtime here). Zeroes `*f` first; `f->lib` stays NULL.
NkXrRefusal nk_xr_load_from_proc(NkXrFns *f, PFN_xrGetInstanceProcAddr get_instance_proc_addr);

// Resolve the instance functions with the created instance. Returns 0, or -1 when
// any is missing.
int nk_xr_load_instance_fns(NkXrFns *f, XrInstance instance);

// Close the loader library if `nk_xr_load` opened one, and zero the table.
void nk_xr_unload(NkXrFns *f);

// Find a usable HMD: enumerate the instance extensions and require
// XR_KHR_vulkan_enable2, create an instance with it, resolve the instance functions
// and get the head-mounted-display system. `f` came from nk_xr_load() or
// nk_xr_load_from_proc(). On NK_XR_OK, `*instance` and `*system` are set and the
// caller owns the instance (f->DestroyInstance); on any refusal nothing is left
// open and the outputs are untouched. `app_name` may be NULL ("Nakagawa Recomp").
// The instance is first requested at XR_CURRENT_API_VERSION; on
// XR_ERROR_API_VERSION_UNSUPPORTED it is requested once more at 1.0.0 (see
// f->api_version). The environment blend modes are enumerated once here for the system;
// a runtime that reports none refuses with NK_XR_REFUSE_INSTANCE.
NkXrRefusal nk_xr_probe(NkXrFns *f, const char *app_name, XrInstance *instance, XrSystemId *system);

// What the caller must do after a session state event.
typedef enum NkXrAction {
    NK_XR_ACT_NONE = 0,
    NK_XR_ACT_BEGIN_SESSION,    // xrBeginSession, then run frames
    NK_XR_ACT_FRAMES_HIDDEN,    // run frames, submit no layers (SYNCHRONIZED)
    NK_XR_ACT_FRAMES_SHOWN,     // run frames, submit layers (VISIBLE, FOCUSED)
    NK_XR_ACT_END_SESSION,      // stop frames, xrEndSession (STOPPING)
    NK_XR_ACT_DESTROY_SESSION,  // stop frames, xrDestroySession; a new session may be made later (LOSS_PENDING)
    NK_XR_ACT_DESTROY_AND_EXIT, // stop frames, xrDestroySession, leave VR (EXITING)
    NK_XR_ACT_EXIT              // leave VR; the session is already destroyed, no OpenXR call (EXITING after LOSS_PENDING)
} NkXrAction;

// Session lifecycle bookkeeping, driven only by nk_xr_session_next() and
// nk_xr_do_action().
typedef struct NkXrRun {
    XrSessionState state;  // last known state (XR_SESSION_STATE_UNKNOWN before the first event)
    int running;           // the session is begun and frames are to be run
    int layers_shown;      // frames carry layers (the state is VISIBLE or FOCUSED)
    int may_retry;         // the session was lost; creating a new one later is allowed
    int done;              // the runtime asked the application to leave VR
    int session_destroyed; // a destroy action was returned and no new session has reached IDLE or READY since
} NkXrRun;

void nk_xr_run_init(NkXrRun *run);

// The session lifecycle as a pure function of the state events: updates `*run` and
// returns the action for the caller to perform. Follows the OpenXR specification,
// chapter "Session", sections "Session Lifecycle" and "Session States":
//   IDLE                     nothing to do
//   READY                    begin the session (once) and start the frame loop
//   SYNCHRONIZED             frames run, layers are not shown
//   VISIBLE, FOCUSED         frames run, layers are shown
//   STOPPING                 stop frames and end the session
//   LOSS_PENDING             destroy the session; a new one may be created later
//   EXITING                  destroy the session and leave; after LOSS_PENDING the
//                            session is already destroyed, so the action is the plain
//                            NK_XR_ACT_EXIT (no xrDestroySession, never a second destroy)
// A state outside XR_SESSION_STATE_IDLE..EXITING, or a NULL `run`, changes nothing
// and returns NK_XR_ACT_NONE. Frame and end actions are returned only while the
// session is running (a state event the runtime sends in the wrong order cannot make
// the caller call xrEndSession on a session that was never begun).
NkXrAction nk_xr_session_next(NkXrRun *run, XrSessionState new_state);

// Perform `action` on `session` through the table and update `*run`. Returns the
// result of the OpenXR call (XR_SUCCESS when the action has no call). When
// xrBeginSession fails, `run->running` stays 0 and the result is returned for the
// caller to report; after a destroy action the session handle is no longer valid.
// Returns XR_ERROR_FUNCTION_UNSUPPORTED, making no call, when the table lacks the
// function the action needs (a table whose instance functions were never resolved).
XrResult nk_xr_do_action(const NkXrFns *f, XrSession session, NkXrRun *run, NkXrAction action);

// What one pass of the frame loop saw.
typedef struct NkXrFrame {
    XrTime display_time;       // predicted display time of this frame
    XrDuration display_period;
    int should_render;         // the runtime's shouldRender
    int slot;                  // mailbox slot holding the latest published frame; -1 before the first publish
    uint64_t frame_id;         // its frame id (0 when slot is -1)
    int show;                  // layers would carry slot: layers_shown && should_render && slot >= 0
    uint32_t layers_submitted; // layers passed to xrEndFrame
} NkXrFrame;

// One iteration of the XR frame loop, on the caller's XR thread: xrWaitFrame,
// xrBeginFrame, take the mailbox's latest slot, xrEndFrame. Slice A submits no
// layer (layers_submitted is 0 in every case): the quad layer that shows the slot
// is added with the presenter tap. Returns the first failing OpenXR result;
// XR_ERROR_SESSION_NOT_RUNNING (no call made) when `run->running` is 0 and
// XR_ERROR_VALIDATION_FAILURE for a NULL argument; XR_ERROR_FUNCTION_UNSUPPORTED (no
// call made) when the table lacks WaitFrame, BeginFrame or EndFrame. This function is the mailbox
// consumer: it never calls the producer's functions.
XrResult nk_xr_frame_once(const NkXrFns *f, XrSession session, const NkXrRun *run, NkXrMailbox *mailbox,
                          NkXrFrame *out);

#ifdef __cplusplus
}
#endif

#endif
