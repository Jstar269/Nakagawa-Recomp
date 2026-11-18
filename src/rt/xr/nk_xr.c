// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// OpenXR module loaded at run time; see nk_xr.h.

#include "xr/nk_xr.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void *nk_xr_lib_open(const char *name) {
#ifdef _WIN32
    // Only the application directory and System32: never the current directory or PATH.
    return (void *)LoadLibraryExA(name, NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

// The symbol as a generic function pointer. ISO C defines no conversion between object
// and function pointers: GetProcAddress already returns a function pointer, and the
// object pointer dlsym returns is copied bytewise, which POSIX guarantees for dlsym.
static PFN_xrVoidFunction nk_xr_lib_sym(void *lib, const char *name) {
#ifdef _WIN32
    return (PFN_xrVoidFunction)GetProcAddress((HMODULE)lib, name);
#else
    void *sym = dlsym(lib, name);
    PFN_xrVoidFunction fn = NULL;
    _Static_assert(sizeof(fn) == sizeof(sym), "dlsym result must fit a function pointer");
    memcpy(&fn, &sym, sizeof(fn));
    return fn;
#endif
}

static void nk_xr_lib_close(void *lib) {
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

const char *nk_xr_refusal_name(NkXrRefusal r) {
    switch (r) {
    case NK_XR_OK:
        return "ok";
    case NK_XR_REFUSE_NO_RUNTIME:
        return "no OpenXR runtime";
    case NK_XR_REFUSE_NO_VULKAN_ENABLE2:
        return "runtime lacks XR_KHR_vulkan_enable2";
    case NK_XR_REFUSE_INSTANCE:
        return "instance creation failed";
    case NK_XR_REFUSE_NO_HMD:
        return "no HMD";
    case NK_XR_REFUSE_OUT_OF_MEMORY:
        return "out of memory";
    case NK_XR_REFUSE_GET_SYSTEM:
        return "runtime failed in xrGetSystem";
    }
    return "unknown refusal";
}

// Resolve one function by name. The proc-address call fills a generic function
// pointer; the cast to the specific PFN type is the documented OpenXR usage.
#define NK_XR_RESOLVE(gipa, instance, f, member, bad)                                  \
    do {                                                                               \
        PFN_xrVoidFunction nk_xr_fn_ = NULL;                                           \
        XrResult nk_xr_r_ = (gipa)((instance), "xr" #member, &nk_xr_fn_);              \
        if (XR_FAILED(nk_xr_r_) || nk_xr_fn_ == NULL) {                                \
            (f)->member = NULL;                                                        \
            (bad)++;                                                                   \
        } else {                                                                       \
            (f)->member = (PFN_xr##member)nk_xr_fn_;                                   \
        }                                                                              \
    } while (0)

NkXrRefusal nk_xr_load_from_proc(NkXrFns *f, PFN_xrGetInstanceProcAddr get_instance_proc_addr) {
    memset(f, 0, sizeof(*f));
    if (get_instance_proc_addr == NULL) {
        return NK_XR_REFUSE_NO_RUNTIME;
    }
    f->GetInstanceProcAddr = get_instance_proc_addr;
    int bad = 0;
    NK_XR_RESOLVE(get_instance_proc_addr, XR_NULL_HANDLE, f, EnumerateInstanceExtensionProperties, bad);
    NK_XR_RESOLVE(get_instance_proc_addr, XR_NULL_HANDLE, f, CreateInstance, bad);
    if (bad != 0) {
        memset(f, 0, sizeof(*f));
        return NK_XR_REFUSE_NO_RUNTIME;
    }
    return NK_XR_OK;
}

NkXrRefusal nk_xr_load(NkXrFns *f, const char *libname) {
    memset(f, 0, sizeof(*f));
    void *lib = nk_xr_lib_open(libname != NULL ? libname : NK_XR_LOADER_NAME);
    if (lib == NULL) {
        return NK_XR_REFUSE_NO_RUNTIME;
    }
    // The symbol has the PFN_xrGetInstanceProcAddr signature.
    PFN_xrVoidFunction sym = nk_xr_lib_sym(lib, "xrGetInstanceProcAddr");
    NkXrRefusal r = nk_xr_load_from_proc(f, (PFN_xrGetInstanceProcAddr)sym);
    if (r != NK_XR_OK) {
        nk_xr_lib_close(lib);
        return r;
    }
    f->lib = lib;
    return NK_XR_OK;
}

int nk_xr_load_instance_fns(NkXrFns *f, XrInstance instance) {
    if (f->GetInstanceProcAddr == NULL) {
        return -1;
    }
    PFN_xrGetInstanceProcAddr gipa = f->GetInstanceProcAddr;
    int bad = 0;
    NK_XR_RESOLVE(gipa, instance, f, DestroyInstance, bad);
    NK_XR_RESOLVE(gipa, instance, f, GetSystem, bad);
    NK_XR_RESOLVE(gipa, instance, f, PollEvent, bad);
    NK_XR_RESOLVE(gipa, instance, f, CreateSession, bad);
    NK_XR_RESOLVE(gipa, instance, f, BeginSession, bad);
    NK_XR_RESOLVE(gipa, instance, f, EndSession, bad);
    NK_XR_RESOLVE(gipa, instance, f, DestroySession, bad);
    NK_XR_RESOLVE(gipa, instance, f, RequestExitSession, bad);
    NK_XR_RESOLVE(gipa, instance, f, WaitFrame, bad);
    NK_XR_RESOLVE(gipa, instance, f, BeginFrame, bad);
    NK_XR_RESOLVE(gipa, instance, f, EndFrame, bad);
    NK_XR_RESOLVE(gipa, instance, f, EnumerateEnvironmentBlendModes, bad);
    return bad == 0 ? 0 : -1;
}

void nk_xr_unload(NkXrFns *f) {
    if (f->lib != NULL) {
        nk_xr_lib_close(f->lib);
    }
    memset(f, 0, sizeof(*f));
}

// Returns 1 when the runtime offers XR_KHR_vulkan_enable2, 0 when it does not,
// -1 when the enumeration fails and -2 when allocation fails. The list can grow between
// the count call and the fill call (an API layer or a runtime switch); the fill call then
// reports XR_ERROR_SIZE_INSUFFICIENT with the new count, and the fill is asked again at
// that count, at most NK_XR_ENUM_ATTEMPTS times.
static int nk_xr_has_vulkan_enable2(const NkXrFns *f) {
    uint32_t count = 0;
    if (XR_FAILED(f->EnumerateInstanceExtensionProperties(NULL, 0, &count, NULL))) {
        return -1;
    }
    for (int attempt = 0; attempt < NK_XR_ENUM_ATTEMPTS; attempt++) {
        if (count == 0) {
            return 0;
        }
        XrExtensionProperties *props = (XrExtensionProperties *)calloc(count, sizeof(*props));
        if (props == NULL) {
            return -2;
        }
        for (uint32_t i = 0; i < count; i++) {
            props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        }
        uint32_t got = 0;
        XrResult r = f->EnumerateInstanceExtensionProperties(NULL, count, &got, props);
        if (XR_SUCCEEDED(r)) {
            int found = 0;
            for (uint32_t i = 0; i < got && i < count; i++) {
                if (strcmp(props[i].extensionName, NK_XR_EXT_VULKAN_ENABLE2) == 0) {
                    found = 1;
                    break;
                }
            }
            free(props);
            return found;
        }
        free(props);
        if (r != XR_ERROR_SIZE_INSUFFICIENT) {
            return -1;
        }
        count = got;  // the count the runtime needs now
    }
    return -1;  // the list changed on every call
}

// Pick the blend mode: OPAQUE when the runtime offers it, else its first mode.
// Returns 0 on success, -1 when the enumeration fails or reports no mode, -2 when
// allocation fails. A fill call that reports XR_ERROR_SIZE_INSUFFICIENT (a mode added
// since the count call) is asked again at the new count, as for the extensions.
static int nk_xr_pick_blend_mode(const NkXrFns *f, XrInstance inst, XrSystemId sys, XrEnvironmentBlendMode *out) {
    uint32_t count = 0;
    if (XR_FAILED(f->EnumerateEnvironmentBlendModes(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count,
                                                    NULL))) {
        return -1;
    }
    for (int attempt = 0; attempt < NK_XR_ENUM_ATTEMPTS; attempt++) {
        if (count == 0) {
            return -1;
        }
        XrEnvironmentBlendMode *modes = (XrEnvironmentBlendMode *)calloc(count, sizeof(*modes));
        if (modes == NULL) {
            return -2;
        }
        uint32_t got = 0;
        XrResult r = f->EnumerateEnvironmentBlendModes(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count,
                                                       &got, modes);
        if (r == XR_ERROR_SIZE_INSUFFICIENT) {
            free(modes);
            count = got;  // the count the runtime needs now
            continue;
        }
        int rc = -1;
        if (XR_SUCCEEDED(r) && got > 0 && got <= count) {
            XrEnvironmentBlendMode pick = modes[0];
            for (uint32_t i = 0; i < got; i++) {
                if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
                    pick = modes[i];
                    break;
                }
            }
            *out = pick;
            rc = 0;
        }
        free(modes);
        return rc;
    }
    return -1;  // the list changed on every call
}

// Copy the NUL-terminated `src` into the `cap`-byte field `dst`. The specification asks
// for a NUL-terminated UTF-8 string there, so a name longer than the field is cut on a
// code point boundary: when the first byte left out is a continuation byte (10xxxxxx),
// the cut fell inside a sequence, and that sequence's lead byte and the continuation
// bytes kept before the cut are dropped as well.
static void nk_xr_copy_utf8(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

NkXrRefusal nk_xr_probe(NkXrFns *f, const char *app_name, XrInstance *instance, XrSystemId *system) {
    if (f == NULL || instance == NULL || system == NULL || f->EnumerateInstanceExtensionProperties == NULL ||
        f->CreateInstance == NULL) {
        return NK_XR_REFUSE_NO_RUNTIME;
    }
    int ext = nk_xr_has_vulkan_enable2(f);
    if (ext == -2) {
        return NK_XR_REFUSE_OUT_OF_MEMORY;
    }
    if (ext < 0) {
        return NK_XR_REFUSE_NO_RUNTIME;
    }
    if (ext == 0) {
        return NK_XR_REFUSE_NO_VULKAN_ENABLE2;
    }

    static const char *const extensions[] = {NK_XR_EXT_VULKAN_ENABLE2};
    XrInstanceCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    nk_xr_copy_utf8(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName),
                    app_name != NULL ? app_name : "Nakagawa Recomp");
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "Nakagawa Recomp", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = extensions;

    XrInstance inst = XR_NULL_HANDLE;
    XrResult cr = f->CreateInstance(&ci, &inst);
    if (cr == XR_ERROR_API_VERSION_UNSUPPORTED) {
        // A 1.0 runtime refuses 1.1: ask once more for 1.0.0.
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        inst = XR_NULL_HANDLE;
        cr = f->CreateInstance(&ci, &inst);
    }
    if (XR_FAILED(cr) || inst == XR_NULL_HANDLE) {
        return NK_XR_REFUSE_INSTANCE;
    }
    if (nk_xr_load_instance_fns(f, inst) != 0) {
        if (f->DestroyInstance != NULL) {
            f->DestroyInstance(inst);
        }
        return NK_XR_REFUSE_INSTANCE;
    }

    XrSystemGetInfo gi;
    memset(&gi, 0, sizeof(gi));
    gi.type = XR_TYPE_SYSTEM_GET_INFO;
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrResult sr = f->GetSystem(inst, &gi, &sys);
    f->get_system_result = sr;
    if (XR_FAILED(sr) || sys == XR_NULL_SYSTEM_ID) {
        f->DestroyInstance(inst);
        // Only these say no headset is there: the HMD form factor is not available now or
        // not supported at all, or a success with no system. Any other failure (a lost
        // instance, a runtime failure) is the runtime's, not a missing headset.
        if (XR_SUCCEEDED(sr) || sr == XR_ERROR_FORM_FACTOR_UNAVAILABLE ||
            sr == XR_ERROR_FORM_FACTOR_UNSUPPORTED) {
            return NK_XR_REFUSE_NO_HMD;
        }
        return NK_XR_REFUSE_GET_SYSTEM;
    }
    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    int br = nk_xr_pick_blend_mode(f, inst, sys, &blend);
    if (br != 0) {
        f->DestroyInstance(inst);
        return br == -2 ? NK_XR_REFUSE_OUT_OF_MEMORY : NK_XR_REFUSE_INSTANCE;
    }
    f->api_version = ci.applicationInfo.apiVersion;
    f->blend_mode = blend;
    *instance = inst;
    *system = sys;
    return NK_XR_OK;
}

void nk_xr_run_init(NkXrRun *run) {
    memset(run, 0, sizeof(*run));
    run->state = XR_SESSION_STATE_UNKNOWN;
}

NkXrAction nk_xr_session_next(NkXrRun *run, XrSessionState new_state) {
    if (run == NULL) {
        return NK_XR_ACT_NONE;
    }
    switch (new_state) {
    case XR_SESSION_STATE_IDLE:
        // Not running a frame loop; the runtime may leave this state for READY (or
        // for EXITING after an exit request).
        run->state = new_state;
        run->session_destroyed = 0;  // a new session
        run->layers_shown = 0;
        return NK_XR_ACT_NONE;
    case XR_SESSION_STATE_READY:
        run->state = new_state;
        run->session_destroyed = 0;
        run->layers_shown = 0;
        if (run->running) {
            return NK_XR_ACT_NONE;  // already begun; xrBeginSession twice is an error
        }
        run->running = 1;
        run->may_retry = 0;
        return NK_XR_ACT_BEGIN_SESSION;
    case XR_SESSION_STATE_SYNCHRONIZED:
        run->state = new_state;
        run->layers_shown = 0;
        return run->running ? NK_XR_ACT_FRAMES_HIDDEN : NK_XR_ACT_NONE;
    case XR_SESSION_STATE_VISIBLE:
    case XR_SESSION_STATE_FOCUSED:
        run->state = new_state;
        run->layers_shown = run->running;
        return run->running ? NK_XR_ACT_FRAMES_SHOWN : NK_XR_ACT_NONE;
    case XR_SESSION_STATE_STOPPING:
        run->state = new_state;
        run->layers_shown = 0;
        if (!run->running) {
            return NK_XR_ACT_NONE;  // nothing begun, nothing to end
        }
        run->running = 0;
        return NK_XR_ACT_END_SESSION;
    case XR_SESSION_STATE_LOSS_PENDING:
        run->state = new_state;
        run->layers_shown = 0;
        run->running = 0;
        if (run->session_destroyed) {
            // A repeated or late LOSS_PENDING (after EXITING) for a session already
            // destroyed: never a second xrDestroySession, and done stays as it was.
            return NK_XR_ACT_NONE;
        }
        run->may_retry = 1;
        run->session_destroyed = 1;
        return NK_XR_ACT_DESTROY_SESSION;
    case XR_SESSION_STATE_EXITING:
        run->state = new_state;
        run->layers_shown = 0;
        run->running = 0;
        run->may_retry = 0;
        run->done = 1;
        if (run->session_destroyed) {
            return NK_XR_ACT_EXIT;  // LOSS_PENDING already destroyed it; never twice
        }
        run->session_destroyed = 1;
        return NK_XR_ACT_DESTROY_AND_EXIT;
    default:
        return NK_XR_ACT_NONE;
    }
}

XrResult nk_xr_do_action(const NkXrFns *f, XrSession session, NkXrRun *run, NkXrAction action) {
    if (f == NULL || run == NULL) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // Each calling action first checks that the table has its function. When it does
    // not, no call is made, and what nk_xr_session_next() recorded for that action is
    // undone so `*run` matches what happened: the session is not begun (a later READY
    // asks for the begin again) or not destroyed (a later EXITING still asks for the
    // destroy). nk_xr_session_next() has already stopped the frames for STOPPING,
    // LOSS_PENDING and EXITING; the end and destroy actions clear running and
    // layers_shown again so a caller that performs one directly, not from a state event,
    // also stops running frames on an ended or destroyed session.
    switch (action) {
    case NK_XR_ACT_BEGIN_SESSION: {
        if (f->BeginSession == NULL) {
            run->running = 0;
            run->layers_shown = 0;
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        XrSessionBeginInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.type = XR_TYPE_SESSION_BEGIN_INFO;
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XrResult r = f->BeginSession(session, &bi);
        if (XR_FAILED(r)) {
            run->running = 0;
            run->layers_shown = 0;
        }
        return r;
    }
    case NK_XR_ACT_END_SESSION:
        if (f->EndSession == NULL) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        run->running = 0;
        run->layers_shown = 0;
        return f->EndSession(session);
    case NK_XR_ACT_DESTROY_SESSION:
    case NK_XR_ACT_DESTROY_AND_EXIT:
        if (f->DestroySession == NULL) {
            run->session_destroyed = 0;
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        run->running = 0;
        run->layers_shown = 0;
        return f->DestroySession(session);
    case NK_XR_ACT_NONE:
    case NK_XR_ACT_FRAMES_HIDDEN:
    case NK_XR_ACT_FRAMES_SHOWN:
    case NK_XR_ACT_EXIT:
        return XR_SUCCESS;  // no OpenXR call: the frame loop runs nk_xr_frame_once()
    }
    return XR_ERROR_VALIDATION_FAILURE;
}

XrResult nk_xr_frame_once(const NkXrFns *f, XrSession session, const NkXrRun *run, NkXrMailbox *mailbox,
                          NkXrFrame *out) {
    if (f == NULL || run == NULL || mailbox == NULL || out == NULL || session == XR_NULL_HANDLE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    memset(out, 0, sizeof(*out));
    out->slot = -1;
    if (f->WaitFrame == NULL || f->BeginFrame == NULL || f->EndFrame == NULL) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (!run->running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    XrFrameWaitInfo wi;
    memset(&wi, 0, sizeof(wi));
    wi.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState fs;
    memset(&fs, 0, sizeof(fs));
    fs.type = XR_TYPE_FRAME_STATE;
    XrResult r = f->WaitFrame(session, &wi, &fs);
    if (XR_FAILED(r)) {
        return r;
    }
    out->display_time = fs.predictedDisplayTime;
    out->display_period = fs.predictedDisplayPeriod;
    out->should_render = fs.shouldRender ? 1 : 0;

    XrFrameBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.type = XR_TYPE_FRAME_BEGIN_INFO;
    r = f->BeginFrame(session, &bi);
    if (XR_FAILED(r)) {
        return r;
    }

    // The latest published frame, or the previous one when nothing new arrived. The
    // acquire never waits on the producer.
    uint64_t id = 0;
    int slot = nk_xr_mailbox_acquire(mailbox, &id);
    out->slot = slot;
    out->frame_id = slot >= 0 ? id : 0;
    out->show = run->layers_shown && out->should_render && slot >= 0;

    // No layer is submitted yet: the quad layer that shows `slot` comes with the
    // presenter tap, which supplies the swapchain it is drawn into. Until then every
    // frame ends with zero layers, which the runtime accepts in any running state.
    XrFrameEndInfo ei;
    memset(&ei, 0, sizeof(ei));
    ei.type = XR_TYPE_FRAME_END_INFO;
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = f->blend_mode != 0 ? f->blend_mode : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = 0;
    ei.layers = NULL;
    out->layers_submitted = 0;
    return f->EndFrame(session, &ei);
}
