// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// xr-selftest: the VR module (src/rt/xr/) against a FAKE OpenXR runtime. No headset,
// no loader and no game are needed. A fake xrGetInstanceProcAddr hands out fake
// functions with scripted results; the test checks the named refusals, the session
// state machine, the latest-frame mailbox, that the frame loop cannot change what the
// producer sees, and the layout maths.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "xr/nk_xr.h"
#include "xr/nk_xr_layout.h"
#include "xr/nk_xr_mailbox.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// The fake runtime.

#define FAKE_INSTANCE ((XrInstance)(uintptr_t)0x1001)
#define FAKE_SESSION ((XrSession)(uintptr_t)0x2002)
#define FAKE_SYSTEM ((XrSystemId)7)

typedef struct Fake {
    // Scripted results.
    XrResult enum_result;
    int has_vulkan_enable2;
    XrResult create_result;
    XrResult get_system_result;
    XrResult begin_session_result;
    XrResult wait_result;
    XrResult begin_frame_result;
    XrResult end_frame_result;
    XrBool32 should_render;
    const char *omit;  // function name the fake proc address refuses to resolve
    int reject_api_11;  // xrCreateInstance refuses an apiVersion of 1.1 or newer
    XrEnvironmentBlendMode blend_modes[4];  // what the runtime offers, in its order
    uint32_t n_blend_modes;
    XrResult blend_result;
    int n_blend_enum;
    XrVersion api_seen[4];  // apiVersion of each xrCreateInstance call
    XrEnvironmentBlendMode last_end_blend;
    // Recorded calls.
    int n_enum, n_create, n_destroy_instance, n_get_system;
    int n_begin_session, n_end_session, n_destroy_session;
    int n_wait, n_begin_frame, n_end_frame;
    uint64_t stall;  // grows by a large step in every xrWaitFrame (a long wait)
    uint32_t last_end_layer_count;
    const void *last_end_layers;
    char order[64];  // W, B, E in call order
    int bad_args;    // a fake saw an argument the OpenXR specification forbids
} Fake;

static Fake fk;

static void fake_reset(void) {
    memset(&fk, 0, sizeof(fk));
    fk.enum_result = XR_SUCCESS;
    fk.has_vulkan_enable2 = 1;
    fk.create_result = XR_SUCCESS;
    fk.get_system_result = XR_SUCCESS;
    fk.begin_session_result = XR_SUCCESS;
    fk.wait_result = XR_SUCCESS;
    fk.begin_frame_result = XR_SUCCESS;
    fk.end_frame_result = XR_SUCCESS;
    fk.should_render = XR_TRUE;
    fk.blend_result = XR_SUCCESS;
    fk.blend_modes[0] = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;  // OPAQUE is not first: the choice matters
    fk.blend_modes[1] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fk.n_blend_modes = 2;
}

static void fake_log(char c) {
    size_t n = strlen(fk.order);
    if (n + 1 < sizeof(fk.order)) {
        fk.order[n] = c;
        fk.order[n + 1] = '\0';
    }
}

static XrResult XRAPI_CALL fake_EnumerateInstanceExtensionProperties(const char *layerName, uint32_t capacityInput,
                                                                      uint32_t *countOutput,
                                                                      XrExtensionProperties *properties) {
    fk.n_enum++;
    if (layerName != NULL || countOutput == NULL) {
        fk.bad_args++;
    }
    if (XR_FAILED(fk.enum_result)) {
        return fk.enum_result;
    }
    const char *names[2] = {"XR_KHR_opengl_enable", "XR_KHR_vulkan_enable2"};
    uint32_t count = fk.has_vulkan_enable2 ? 2u : 1u;
    *countOutput = count;
    if (capacityInput == 0) {
        return XR_SUCCESS;
    }
    if (capacityInput < count) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (properties[i].type != XR_TYPE_EXTENSION_PROPERTIES) {
            fk.bad_args++;
        }
        strcpy(properties[i].extensionName, names[i]);
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_CreateInstance(const XrInstanceCreateInfo *createInfo, XrInstance *instance) {
    if (fk.n_create < 4 && createInfo != NULL) {
        fk.api_seen[fk.n_create] = createInfo->applicationInfo.apiVersion;
    }
    fk.n_create++;
    if (createInfo == NULL || createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO || createInfo->enabledExtensionCount != 1 ||
        strcmp(createInfo->enabledExtensionNames[0], NK_XR_EXT_VULKAN_ENABLE2) != 0 ||
        (createInfo->applicationInfo.apiVersion != XR_CURRENT_API_VERSION &&
         createInfo->applicationInfo.apiVersion != XR_MAKE_VERSION(1, 0, 0))) {
        fk.bad_args++;
    }
    if (fk.reject_api_11 && createInfo != NULL && createInfo->applicationInfo.apiVersion >= XR_MAKE_VERSION(1, 1, 0)) {
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }
    if (XR_FAILED(fk.create_result)) {
        return fk.create_result;
    }
    *instance = FAKE_INSTANCE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_DestroyInstance(XrInstance instance) {
    fk.n_destroy_instance++;
    if (instance != FAKE_INSTANCE) {
        fk.bad_args++;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_GetSystem(XrInstance instance, const XrSystemGetInfo *getInfo, XrSystemId *systemId) {
    fk.n_get_system++;
    if (instance != FAKE_INSTANCE || getInfo == NULL || getInfo->type != XR_TYPE_SYSTEM_GET_INFO ||
        getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        fk.bad_args++;
    }
    if (XR_FAILED(fk.get_system_result)) {
        return fk.get_system_result;
    }
    *systemId = FAKE_SYSTEM;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_EnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                                                XrViewConfigurationType viewConfigurationType,
                                                                uint32_t capacityInput, uint32_t *countOutput,
                                                                XrEnvironmentBlendMode *modes) {
    fk.n_blend_enum++;
    if (instance != FAKE_INSTANCE || systemId != FAKE_SYSTEM ||
        viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO || countOutput == NULL) {
        fk.bad_args++;
    }
    if (XR_FAILED(fk.blend_result)) {
        return fk.blend_result;
    }
    *countOutput = fk.n_blend_modes;
    if (capacityInput == 0) {
        return XR_SUCCESS;
    }
    if (capacityInput < fk.n_blend_modes) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (uint32_t i = 0; i < fk.n_blend_modes; i++) {
        modes[i] = fk.blend_modes[i];
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_PollEvent(XrInstance instance, XrEventDataBuffer *eventData) {
    (void)instance;
    (void)eventData;
    return XR_EVENT_UNAVAILABLE;
}

static XrResult XRAPI_CALL fake_CreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo,
                                              XrSession *session) {
    (void)instance;
    (void)createInfo;
    *session = FAKE_SESSION;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_BeginSession(XrSession session, const XrSessionBeginInfo *beginInfo) {
    fk.n_begin_session++;
    if (session != FAKE_SESSION || beginInfo == NULL || beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO ||
        beginInfo->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        fk.bad_args++;
    }
    return fk.begin_session_result;
}

static XrResult XRAPI_CALL fake_EndSession(XrSession session) {
    fk.n_end_session++;
    if (session != FAKE_SESSION) {
        fk.bad_args++;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_DestroySession(XrSession session) {
    fk.n_destroy_session++;
    if (session != FAKE_SESSION) {
        fk.bad_args++;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_RequestExitSession(XrSession session) {
    (void)session;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_WaitFrame(XrSession session, const XrFrameWaitInfo *frameWaitInfo,
                                          XrFrameState *frameState) {
    fk.n_wait++;
    fake_log('W');
    // A long wait for the next headset frame: the fake does not sleep, it records
    // that the call "took" a large amount of time.
    fk.stall += 1000000u;
    if (session != FAKE_SESSION || frameWaitInfo == NULL || frameWaitInfo->type != XR_TYPE_FRAME_WAIT_INFO ||
        frameState == NULL || frameState->type != XR_TYPE_FRAME_STATE) {
        fk.bad_args++;
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (XR_FAILED(fk.wait_result)) {
        return fk.wait_result;
    }
    frameState->predictedDisplayTime = (XrTime)(fk.n_wait * 11111111);
    frameState->predictedDisplayPeriod = 11111111;
    frameState->shouldRender = fk.should_render;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL fake_BeginFrame(XrSession session, const XrFrameBeginInfo *frameBeginInfo) {
    fk.n_begin_frame++;
    fake_log('B');
    if (session != FAKE_SESSION || frameBeginInfo == NULL || frameBeginInfo->type != XR_TYPE_FRAME_BEGIN_INFO) {
        fk.bad_args++;
    }
    return fk.begin_frame_result;
}

static XrResult XRAPI_CALL fake_EndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo) {
    fk.n_end_frame++;
    fake_log('E');
    if (session != FAKE_SESSION || frameEndInfo == NULL || frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
        fk.bad_args++;
        return XR_ERROR_VALIDATION_FAILURE;
    }
    fk.last_end_layer_count = frameEndInfo->layerCount;
    fk.last_end_layers = frameEndInfo->layers;
    fk.last_end_blend = frameEndInfo->environmentBlendMode;
    return fk.end_frame_result;
}

typedef struct FakeFn {
    const char *name;
    PFN_xrVoidFunction fn;
    int global;  // a global function: the instance argument must be XR_NULL_HANDLE
} FakeFn;

#define FAKE_ENTRY(n, g) {"xr" #n, (PFN_xrVoidFunction)fake_##n, (g)}

static XrResult XRAPI_CALL fake_GetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    static const FakeFn table[] = {
        FAKE_ENTRY(EnumerateInstanceExtensionProperties, 1),
        FAKE_ENTRY(CreateInstance, 1),
        FAKE_ENTRY(DestroyInstance, 0),
        FAKE_ENTRY(GetSystem, 0),
        FAKE_ENTRY(PollEvent, 0),
        FAKE_ENTRY(CreateSession, 0),
        FAKE_ENTRY(BeginSession, 0),
        FAKE_ENTRY(EndSession, 0),
        FAKE_ENTRY(DestroySession, 0),
        FAKE_ENTRY(RequestExitSession, 0),
        FAKE_ENTRY(WaitFrame, 0),
        FAKE_ENTRY(BeginFrame, 0),
        FAKE_ENTRY(EndFrame, 0),
        FAKE_ENTRY(EnumerateEnvironmentBlendModes, 0),
    };
    *function = NULL;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(table[i].name, name) != 0) {
            continue;
        }
        if (fk.omit != NULL && strcmp(fk.omit, name) == 0) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        if (table[i].global ? (instance != XR_NULL_HANDLE) : (instance != FAKE_INSTANCE)) {
            fk.bad_args++;
            return XR_ERROR_HANDLE_INVALID;
        }
        *function = table[i].fn;
        return XR_SUCCESS;
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// A loaded fake table with the instance functions already resolved.
static NkXrFns fake_table(void) {
    NkXrFns f;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(nk_xr_load_instance_fns(&f, FAKE_INSTANCE) == 0);
    return f;
}

// ---------------------------------------------------------------------------
// Refusals.

static void test_refusals(void) {
    NkXrFns f;
    XrInstance inst = (XrInstance)(uintptr_t)0xdead;
    XrSystemId sys = 99;

    // Names.
    CHECK(strcmp(nk_xr_refusal_name(NK_XR_OK), "ok") == 0);
    CHECK(strcmp(nk_xr_refusal_name(NK_XR_REFUSE_NO_RUNTIME), "no OpenXR runtime") == 0);
    CHECK(strcmp(nk_xr_refusal_name(NK_XR_REFUSE_NO_VULKAN_ENABLE2), "runtime lacks XR_KHR_vulkan_enable2") == 0);
    CHECK(strcmp(nk_xr_refusal_name(NK_XR_REFUSE_INSTANCE), "instance creation failed") == 0);
    CHECK(strcmp(nk_xr_refusal_name(NK_XR_REFUSE_NO_HMD), "no HMD") == 0);
    CHECK(strcmp(nk_xr_refusal_name(NK_XR_REFUSE_OUT_OF_MEMORY), "out of memory") == 0);
    CHECK(strcmp(nk_xr_refusal_name((NkXrRefusal)1234), "unknown refusal") == 0);

    // No loader: a library name that does not exist.
    memset(&f, 0xAA, sizeof(f));
    NkXrRefusal r = nk_xr_load(&f, "nk_xr_no_such_loader_library_791");
    CHECK(r == NK_XR_REFUSE_NO_RUNTIME);
    CHECK(strcmp(nk_xr_refusal_name(r), "no OpenXR runtime") == 0);
    CHECK(f.lib == NULL && f.GetInstanceProcAddr == NULL && f.CreateInstance == NULL);
    // A probe on the refused table refuses in the same way and touches nothing.
    CHECK(nk_xr_probe(&f, "test", &inst, &sys) == NK_XR_REFUSE_NO_RUNTIME);
    CHECK(inst == (XrInstance)(uintptr_t)0xdead && sys == 99);
    nk_xr_unload(&f);  // safe on a refused table

    // A runtime whose proc address cannot resolve a global function.
    fake_reset();
    CHECK(nk_xr_load_from_proc(&f, NULL) == NK_XR_REFUSE_NO_RUNTIME);
    fk.omit = "xrCreateInstance";
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_REFUSE_NO_RUNTIME);
    CHECK(f.GetInstanceProcAddr == NULL);

    // Enumeration fails: no active runtime behind the loader.
    fake_reset();
    fk.enum_result = XR_ERROR_RUNTIME_UNAVAILABLE;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    r = nk_xr_probe(&f, "test", &inst, &sys);
    CHECK(r == NK_XR_REFUSE_NO_RUNTIME);
    CHECK(fk.n_create == 0 && fk.n_destroy_instance == 0);

    // Runtime without XR_KHR_vulkan_enable2.
    fake_reset();
    fk.has_vulkan_enable2 = 0;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    r = nk_xr_probe(&f, "test", &inst, &sys);
    CHECK(r == NK_XR_REFUSE_NO_VULKAN_ENABLE2);
    CHECK(strcmp(nk_xr_refusal_name(r), "runtime lacks XR_KHR_vulkan_enable2") == 0);
    CHECK(fk.n_create == 0);

    // Instance creation fails.
    fake_reset();
    fk.create_result = XR_ERROR_INITIALIZATION_FAILED;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    r = nk_xr_probe(&f, "test", &inst, &sys);
    CHECK(r == NK_XR_REFUSE_INSTANCE);
    CHECK(strcmp(nk_xr_refusal_name(r), "instance creation failed") == 0);
    CHECK(fk.n_create == 1 && fk.n_destroy_instance == 0);

    // An instance function cannot be resolved: the instance is destroyed again.
    fake_reset();
    fk.omit = "xrGetSystem";
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    r = nk_xr_probe(&f, "test", &inst, &sys);
    CHECK(r == NK_XR_REFUSE_INSTANCE);
    CHECK(fk.n_create == 1 && fk.n_destroy_instance == 1 && fk.n_get_system == 0);

    // No head-mounted display: the instance is destroyed again.
    fake_reset();
    fk.get_system_result = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    r = nk_xr_probe(&f, "test", &inst, &sys);
    CHECK(r == NK_XR_REFUSE_NO_HMD);
    CHECK(strcmp(nk_xr_refusal_name(r), "no HMD") == 0);
    CHECK(fk.n_get_system == 1 && fk.n_destroy_instance == 1);
    CHECK(inst == (XrInstance)(uintptr_t)0xdead && sys == 99);  // outputs untouched on every refusal

    // Success.
    fake_reset();
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(f.lib == NULL && f.GetInstanceProcAddr == fake_GetInstanceProcAddr);
    r = nk_xr_probe(&f, NULL, &inst, &sys);
    CHECK(r == NK_XR_OK);
    CHECK(inst == FAKE_INSTANCE && sys == FAKE_SYSTEM);
    CHECK(fk.n_create == 1 && fk.n_get_system == 1 && fk.n_destroy_instance == 0);
    CHECK(f.WaitFrame == fake_WaitFrame && f.EndFrame == fake_EndFrame);
    CHECK(fk.bad_args == 0);
}

// ---------------------------------------------------------------------------
// API version fallback, blend mode choice, NULL tables.

static void test_api_version_fallback(void) {
    NkXrFns f;
    XrInstance inst = XR_NULL_HANDLE;
    XrSystemId sys = 0;

    // A 1.1 runtime grants the current version on the first try.
    fake_reset();
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(nk_xr_probe(&f, "test", &inst, &sys) == NK_XR_OK);
    CHECK(fk.n_create == 1 && f.api_version == XR_CURRENT_API_VERSION);

    // A 1.0 runtime refuses 1.1: exactly one retry at 1.0.0, and the grant is recorded.
    fake_reset();
    fk.reject_api_11 = 1;
    inst = XR_NULL_HANDLE;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(nk_xr_probe(&f, "test", &inst, &sys) == NK_XR_OK);
    CHECK(inst == FAKE_INSTANCE && sys == FAKE_SYSTEM);
    CHECK(fk.n_create == 2);
    CHECK(fk.api_seen[0] == XR_CURRENT_API_VERSION && fk.api_seen[1] == XR_MAKE_VERSION(1, 0, 0));
    CHECK(f.api_version == XR_MAKE_VERSION(1, 0, 0));
    CHECK(fk.bad_args == 0);

    // Any other creation failure is not retried.
    fake_reset();
    fk.create_result = XR_ERROR_RUNTIME_FAILURE;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(nk_xr_probe(&f, "test", &inst, &sys) == NK_XR_REFUSE_INSTANCE);
    CHECK(fk.n_create == 1);
}

static void test_blend_mode_choice(void) {
    NkXrFns f;
    NkXrRun run;
    NkXrFrame fr;
    NkXrMailbox m;
    XrInstance inst = XR_NULL_HANDLE;
    XrSystemId sys = 0;
    nk_xr_mailbox_init(&m);

    // OPAQUE offered (second in the list): chosen.
    fake_reset();
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(nk_xr_probe(&f, "test", &inst, &sys) == NK_XR_OK);
    CHECK(f.blend_mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    int enums = fk.n_blend_enum;
    CHECK(enums >= 1);
    nk_xr_run_init(&run);
    nk_xr_session_next(&run, XR_SESSION_STATE_READY);
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_SUCCESS);
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_SUCCESS);
    CHECK(fk.last_end_blend == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    CHECK(fk.n_blend_enum == enums);  // enumerated once, not per frame

    // A runtime with additive only (a see-through headset): its first mode is used.
    fake_reset();
    fk.blend_modes[0] = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    fk.n_blend_modes = 1;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(nk_xr_probe(&f, "test", &inst, &sys) == NK_XR_OK);
    CHECK(f.blend_mode == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE);
    nk_xr_run_init(&run);
    nk_xr_session_next(&run, XR_SESSION_STATE_READY);
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_SUCCESS);
    CHECK(fk.last_end_blend == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE);
    CHECK(fk.bad_args == 0);

    // A runtime that offers no mode, or fails the query: refused, instance destroyed again.
    fake_reset();
    fk.n_blend_modes = 0;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(nk_xr_probe(&f, "test", &inst, &sys) == NK_XR_REFUSE_INSTANCE);
    CHECK(fk.n_destroy_instance == 1);
    fake_reset();
    fk.blend_result = XR_ERROR_RUNTIME_FAILURE;
    CHECK(nk_xr_load_from_proc(&f, fake_GetInstanceProcAddr) == NK_XR_OK);
    CHECK(nk_xr_probe(&f, "test", &inst, &sys) == NK_XR_REFUSE_INSTANCE);
    CHECK(fk.n_destroy_instance == 1);
}

static void test_null_table_refusals(void) {
    NkXrFns z;
    NkXrRun run;
    NkXrFrame fr;
    NkXrMailbox m;
    memset(&z, 0, sizeof(z));
    nk_xr_mailbox_init(&m);
    nk_xr_run_init(&run);
    nk_xr_session_next(&run, XR_SESSION_STATE_READY);  // running = 1

    // A table whose instance functions were never resolved: a named result, no crash.
    CHECK(nk_xr_do_action(&z, FAKE_SESSION, &run, NK_XR_ACT_BEGIN_SESSION) == XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(nk_xr_do_action(&z, FAKE_SESSION, &run, NK_XR_ACT_END_SESSION) == XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(nk_xr_do_action(&z, FAKE_SESSION, &run, NK_XR_ACT_DESTROY_SESSION) == XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(nk_xr_do_action(&z, FAKE_SESSION, &run, NK_XR_ACT_DESTROY_AND_EXIT) == XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(run.running == 1);  // a refused action changes no bookkeeping
    CHECK(nk_xr_do_action(&z, FAKE_SESSION, &run, NK_XR_ACT_FRAMES_SHOWN) == XR_SUCCESS);  // no call needed
    CHECK(nk_xr_frame_once(&z, FAKE_SESSION, &run, &m, &fr) == XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(fr.slot == -1 && fr.layers_submitted == 0);

    // One missing function is enough.
    fake_reset();
    NkXrFns f = fake_table();
    f.EndFrame = NULL;
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(fk.n_wait == 0);  // refused before any call, so no frame is left begun
}

static void test_loss_then_exit(void) {
    // EXITING after LOSS_PENDING must not destroy the session a second time.
    fake_reset();
    NkXrFns f = fake_table();
    NkXrRun run;
    nk_xr_run_init(&run);
    NkXrAction a = nk_xr_session_next(&run, XR_SESSION_STATE_READY);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, a) == XR_SUCCESS);
    a = nk_xr_session_next(&run, XR_SESSION_STATE_LOSS_PENDING);
    CHECK(a == NK_XR_ACT_DESTROY_SESSION);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, a) == XR_SUCCESS);
    CHECK(fk.n_destroy_session == 1);
    a = nk_xr_session_next(&run, XR_SESSION_STATE_EXITING);
    CHECK(a == NK_XR_ACT_EXIT);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, a) == XR_SUCCESS);
    CHECK(fk.n_destroy_session == 1 && run.done == 1);

    // A new session after the loss that then exits is destroyed once, as before.
    fake_reset();
    nk_xr_run_init(&run);
    nk_xr_session_next(&run, XR_SESSION_STATE_LOSS_PENDING);
    nk_xr_session_next(&run, XR_SESSION_STATE_IDLE);
    a = nk_xr_session_next(&run, XR_SESSION_STATE_EXITING);
    CHECK(a == NK_XR_ACT_DESTROY_AND_EXIT);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, a) == XR_SUCCESS);
    CHECK(fk.n_destroy_session == 1);
}

// ---------------------------------------------------------------------------
// Session state machine.

typedef struct Step {
    XrSessionState state;
    NkXrAction action;
    int running, layers_shown;
} Step;

static void run_steps(const Step *steps, size_t n, NkXrRun *run) {
    for (size_t i = 0; i < n; i++) {
        NkXrAction a = nk_xr_session_next(run, steps[i].state);
        CHECK(a == steps[i].action);
        CHECK(run->state == steps[i].state);
        CHECK(run->running == steps[i].running);
        CHECK(run->layers_shown == steps[i].layers_shown);
    }
}

static void test_state_machine(void) {
    NkXrRun run;

    // The lifecycle in order, a restart after STOPPING, and EXITING from FOCUSED.
    nk_xr_run_init(&run);
    CHECK(run.state == XR_SESSION_STATE_UNKNOWN && !run.running && !run.done);
    const Step full[] = {
        {XR_SESSION_STATE_IDLE, NK_XR_ACT_NONE, 0, 0},
        {XR_SESSION_STATE_READY, NK_XR_ACT_BEGIN_SESSION, 1, 0},
        {XR_SESSION_STATE_READY, NK_XR_ACT_NONE, 1, 0},  // a repeated READY never begins twice
        {XR_SESSION_STATE_SYNCHRONIZED, NK_XR_ACT_FRAMES_HIDDEN, 1, 0},
        {XR_SESSION_STATE_VISIBLE, NK_XR_ACT_FRAMES_SHOWN, 1, 1},
        {XR_SESSION_STATE_FOCUSED, NK_XR_ACT_FRAMES_SHOWN, 1, 1},
        {XR_SESSION_STATE_VISIBLE, NK_XR_ACT_FRAMES_SHOWN, 1, 1},
        {XR_SESSION_STATE_SYNCHRONIZED, NK_XR_ACT_FRAMES_HIDDEN, 1, 0},
        {XR_SESSION_STATE_STOPPING, NK_XR_ACT_END_SESSION, 0, 0},
        {XR_SESSION_STATE_IDLE, NK_XR_ACT_NONE, 0, 0},
        {XR_SESSION_STATE_READY, NK_XR_ACT_BEGIN_SESSION, 1, 0},  // a second run of the session
        {XR_SESSION_STATE_SYNCHRONIZED, NK_XR_ACT_FRAMES_HIDDEN, 1, 0},
        {XR_SESSION_STATE_VISIBLE, NK_XR_ACT_FRAMES_SHOWN, 1, 1},
        {XR_SESSION_STATE_FOCUSED, NK_XR_ACT_FRAMES_SHOWN, 1, 1},
        {XR_SESSION_STATE_EXITING, NK_XR_ACT_DESTROY_AND_EXIT, 0, 0},
    };
    run_steps(full, sizeof(full) / sizeof(full[0]), &run);
    CHECK(run.done == 1 && run.may_retry == 0);

    // LOSS_PENDING from FOCUSED: destroy, a new session may be made later.
    nk_xr_run_init(&run);
    const Step loss[] = {
        {XR_SESSION_STATE_READY, NK_XR_ACT_BEGIN_SESSION, 1, 0},
        {XR_SESSION_STATE_SYNCHRONIZED, NK_XR_ACT_FRAMES_HIDDEN, 1, 0},
        {XR_SESSION_STATE_VISIBLE, NK_XR_ACT_FRAMES_SHOWN, 1, 1},
        {XR_SESSION_STATE_FOCUSED, NK_XR_ACT_FRAMES_SHOWN, 1, 1},
        {XR_SESSION_STATE_LOSS_PENDING, NK_XR_ACT_DESTROY_SESSION, 0, 0},
    };
    run_steps(loss, sizeof(loss) / sizeof(loss[0]), &run);
    CHECK(run.may_retry == 1 && run.done == 0);
    // The retry: a fresh session reaches READY and is begun, which clears may_retry.
    const Step retry[] = {
        {XR_SESSION_STATE_IDLE, NK_XR_ACT_NONE, 0, 0},
        {XR_SESSION_STATE_READY, NK_XR_ACT_BEGIN_SESSION, 1, 0},
    };
    run_steps(retry, sizeof(retry) / sizeof(retry[0]), &run);
    CHECK(run.may_retry == 0);

    // EXITING from VISIBLE.
    nk_xr_run_init(&run);
    const Step exit_visible[] = {
        {XR_SESSION_STATE_READY, NK_XR_ACT_BEGIN_SESSION, 1, 0},
        {XR_SESSION_STATE_SYNCHRONIZED, NK_XR_ACT_FRAMES_HIDDEN, 1, 0},
        {XR_SESSION_STATE_VISIBLE, NK_XR_ACT_FRAMES_SHOWN, 1, 1},
        {XR_SESSION_STATE_EXITING, NK_XR_ACT_DESTROY_AND_EXIT, 0, 0},
    };
    run_steps(exit_visible, sizeof(exit_visible) / sizeof(exit_visible[0]), &run);
    CHECK(run.done == 1);

    // Events in the wrong order never cause an end call on a session that was never begun.
    nk_xr_run_init(&run);
    const Step odd[] = {
        {XR_SESSION_STATE_SYNCHRONIZED, NK_XR_ACT_NONE, 0, 0},
        {XR_SESSION_STATE_VISIBLE, NK_XR_ACT_NONE, 0, 0},
        {XR_SESSION_STATE_STOPPING, NK_XR_ACT_NONE, 0, 0},
    };
    run_steps(odd, sizeof(odd) / sizeof(odd[0]), &run);

    // An unknown state value changes nothing, in any condition.
    const int values[] = {0, 9, 10, 99, -1, 0x7FFFFFFF};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        nk_xr_run_init(&run);
        nk_xr_session_next(&run, XR_SESSION_STATE_READY);
        nk_xr_session_next(&run, XR_SESSION_STATE_FOCUSED);
        NkXrRun before = run;
        CHECK(nk_xr_session_next(&run, (XrSessionState)values[i]) == NK_XR_ACT_NONE);
        CHECK(memcmp(&before, &run, sizeof(run)) == 0);
    }
    CHECK(nk_xr_session_next(NULL, XR_SESSION_STATE_READY) == NK_XR_ACT_NONE);

    // nk_xr_do_action performs the calls the actions name.
    fake_reset();
    NkXrFns f = fake_table();
    nk_xr_run_init(&run);
    NkXrAction a = nk_xr_session_next(&run, XR_SESSION_STATE_READY);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, a) == XR_SUCCESS);
    CHECK(fk.n_begin_session == 1 && run.running == 1);
    a = nk_xr_session_next(&run, XR_SESSION_STATE_STOPPING);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, a) == XR_SUCCESS);
    CHECK(fk.n_end_session == 1 && run.running == 0);
    a = nk_xr_session_next(&run, XR_SESSION_STATE_EXITING);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, a) == XR_SUCCESS);
    CHECK(fk.n_destroy_session == 1 && run.done == 1);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, NK_XR_ACT_FRAMES_SHOWN) == XR_SUCCESS);
    CHECK(fk.n_begin_session == 1 && fk.n_end_session == 1 && fk.n_destroy_session == 1);

    // A failed xrBeginSession leaves the loop stopped and reports the result.
    fake_reset();
    fk.begin_session_result = XR_ERROR_SESSION_RUNNING;
    nk_xr_run_init(&run);
    a = nk_xr_session_next(&run, XR_SESSION_STATE_READY);
    CHECK(nk_xr_do_action(&f, FAKE_SESSION, &run, a) == XR_ERROR_SESSION_RUNNING);
    CHECK(run.running == 0);
    CHECK(fk.bad_args == 0);
}

// ---------------------------------------------------------------------------
// Mailbox.

static void test_mailbox_basic(void) {
    NkXrMailbox m;
    uint64_t id = 77;
    nk_xr_mailbox_init(&m);
    int ws = nk_xr_mailbox_write_slot(&m);
    CHECK(ws >= 0 && ws < NK_XR_MAILBOX_SLOTS);
    CHECK(nk_xr_mailbox_acquire(&m, &id) == -1);  // nothing published yet
    CHECK(id == 77);
    CHECK(nk_xr_mailbox_acquire(&m, NULL) == -1);
    CHECK(nk_xr_mailbox_write_slot(&m) == ws);  // acquiring does not move the producer

    nk_xr_mailbox_publish(&m, 10);
    CHECK(nk_xr_mailbox_write_slot(&m) != ws);
    int s = nk_xr_mailbox_acquire(&m, &id);
    CHECK(s == ws && id == 10);
    CHECK(nk_xr_mailbox_write_slot(&m) != s);

    // Nothing new: the consumer keeps the same slot and id, again and again.
    for (int i = 0; i < 5; i++) {
        uint64_t id2 = 0;
        CHECK(nk_xr_mailbox_acquire(&m, &id2) == s && id2 == 10);
        CHECK(nk_xr_mailbox_write_slot(&m) != s);
    }

    // Several frames published before the next acquire: the consumer gets the latest.
    int w1 = nk_xr_mailbox_write_slot(&m);
    nk_xr_mailbox_publish(&m, 11);
    int w2 = nk_xr_mailbox_write_slot(&m);
    CHECK(w2 != s);
    nk_xr_mailbox_publish(&m, 12);
    int w3 = nk_xr_mailbox_write_slot(&m);
    CHECK(w3 != s && w3 != w2);
    nk_xr_mailbox_publish(&m, 13);
    (void)w1;
    int s2 = nk_xr_mailbox_acquire(&m, &id);
    CHECK(id == 13 && s2 != s);
    CHECK(nk_xr_mailbox_write_slot(&m) != s2);
}

// Exhaustive scripted interleavings: every sequence of 12 producer/consumer steps. Each
// slot has a memory word; the producer writes the frame id into its write slot before
// publishing. A read of a slot must always show the id acquire returned (no torn read),
// and the slot a consumer holds must never be a slot the producer is told to write.
static void test_mailbox_interleavings(void) {
    enum { STEPS = 12 };
    for (unsigned mask = 0; mask < (1u << STEPS); mask++) {
        NkXrMailbox m;
        uint64_t data[NK_XR_MAILBOX_SLOTS] = {0, 0, 0};
        nk_xr_mailbox_init(&m);
        uint64_t next_id = 0, last_published = 0, held_id = 0;
        int held = -1;
        int published_since_acquire = 0;
        int ok = 1;
        for (int step = 0; step < STEPS; step++) {
            if (mask & (1u << step)) {
                int ws = nk_xr_mailbox_write_slot(&m);
                ok &= ws >= 0 && ws < NK_XR_MAILBOX_SLOTS && ws != held;
                if (!ok) {
                    break;
                }
                data[ws] = ++next_id;
                nk_xr_mailbox_publish(&m, next_id);
                last_published = next_id;
                published_since_acquire = 1;
            } else {
                uint64_t id = 0;
                int s = nk_xr_mailbox_acquire(&m, &id);
                if (last_published == 0) {
                    ok &= s == -1;
                } else {
                    ok &= s >= 0 && s < NK_XR_MAILBOX_SLOTS && id == last_published && data[s] == id;
                    if (!published_since_acquire) {
                        ok &= s == held;
                    }
                    held = s;
                    held_id = id;
                    published_since_acquire = 0;
                }
            }
            if (held >= 0) {
                ok &= data[held] == held_id;  // the held slot is never overwritten
                ok &= nk_xr_mailbox_write_slot(&m) != held;
            }
            if (!ok) {
                break;
            }
        }
        g_checks++;
        if (!ok) {
            g_failures++;
            printf("FAIL mailbox interleaving mask %u\n", mask);
            return;
        }
    }
}

static void test_mailbox_producer_never_waits(void) {
    NkXrMailbox m;
    uint64_t data[NK_XR_MAILBOX_SLOTS] = {0, 0, 0};
    nk_xr_mailbox_init(&m);
    // The consumer takes one frame and then holds that slot for the whole run.
    int ws = nk_xr_mailbox_write_slot(&m);
    data[ws] = 1;
    nk_xr_mailbox_publish(&m, 1);
    uint64_t id = 0;
    int held = nk_xr_mailbox_acquire(&m, &id);
    CHECK(held == ws && id == 1);

    enum { N = 1000000 };
    int bad_slot = 0, held_overwritten = 0;
    for (uint64_t i = 2; i < N + 2; i++) {
        int s = nk_xr_mailbox_write_slot(&m);
        if (s < 0 || s >= NK_XR_MAILBOX_SLOTS || s == held) {
            bad_slot++;
        } else {
            data[s] = i;
        }
        nk_xr_mailbox_publish(&m, i);
        if (data[held] != 1) {
            held_overwritten++;
        }
    }
    CHECK(bad_slot == 0);
    CHECK(held_overwritten == 0);
    int last = nk_xr_mailbox_acquire(&m, &id);
    CHECK(last >= 0 && id == N + 1 && data[last] == N + 1);
    CHECK(nk_xr_mailbox_acquire(&m, NULL) == last);
}

// Two real threads. The producer fills the whole slot with a per-frame pattern and
// publishes; the consumer acquires and checks that every word of the slot it holds
// matches the frame id acquire returned, and that frame ids never go backwards. The
// producer never waits and the consumer stops once it has seen the last frame.
//
// NOTE: x86 is strongly ordered, so this test cannot catch a mutant that weakens the
// acq_rel exchanges in nk_xr_mailbox.c to relaxed here. It is a smoke test on x86 and
// a strong guard on weakly ordered CPUs (ARM64 builds, Android #360).
enum { STRESS_FRAMES = 200000, STRESS_WORDS = 64 };

typedef struct Stress {
    NkXrMailbox mb;
    uint64_t slots[NK_XR_MAILBOX_SLOTS][STRESS_WORDS];
    uint64_t torn, backwards, acquires, distinct, last_seen;
} Stress;

static uint64_t stress_word(uint64_t id, int i) {
    return id * 0x9E3779B97F4A7C15ull + (uint64_t)i * 0xD1B54A32D192ED03ull + 1u;
}

static void stress_produce(Stress *s) {
    for (uint64_t id = 1; id <= STRESS_FRAMES; id++) {
        int slot = nk_xr_mailbox_write_slot(&s->mb);
        for (int i = 0; i < STRESS_WORDS; i++) {
            s->slots[slot][i] = stress_word(id, i);
        }
        nk_xr_mailbox_publish(&s->mb, id);
    }
}

#ifdef _WIN32
static DWORD WINAPI stress_thread(LPVOID arg) {
    stress_produce((Stress *)arg);
    return 0;
}
#else
static void *stress_thread(void *arg) {
    stress_produce((Stress *)arg);
    return NULL;
}
#endif

static void test_mailbox_two_threads(void) {
    static Stress s;
    memset(&s, 0, sizeof(s));
    nk_xr_mailbox_init(&s.mb);
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, stress_thread, &s, 0, NULL);
    CHECK(th != NULL);
    if (th == NULL) {
        return;
    }
#else
    pthread_t th;
    int rc = pthread_create(&th, NULL, stress_thread, &s);
    CHECK(rc == 0);
    if (rc != 0) {
        return;
    }
#endif
    while (s.last_seen != STRESS_FRAMES) {
        uint64_t id = 0;
        int slot = nk_xr_mailbox_acquire(&s.mb, &id);
        s.acquires++;
        if (slot < 0) {
            continue;
        }
        if (id < s.last_seen) {
            s.backwards++;
        }
        for (int i = 0; i < STRESS_WORDS; i++) {
            if (s.slots[slot][i] != stress_word(id, i)) {
                s.torn++;
                break;
            }
        }
        if (id != s.last_seen) {
            s.distinct++;
        }
        s.last_seen = id;
    }
#ifdef _WIN32
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    CHECK(s.torn == 0);
    CHECK(s.backwards == 0);
    CHECK(s.distinct >= 1 && s.last_seen == STRESS_FRAMES);
}

// ---------------------------------------------------------------------------
// The frame loop cannot change what the producer sees.

typedef struct ProducerLog {
    int calls;
    int bad;
} ProducerLog;

static void produce(NkXrMailbox *m, ProducerLog *log, uint64_t *data, uint64_t id, int consumer_slot) {
    int s = nk_xr_mailbox_write_slot(m);
    log->calls++;
    if (s < 0 || s >= NK_XR_MAILBOX_SLOTS || s == consumer_slot) {
        log->bad++;
    } else {
        data[s] = id;
    }
    nk_xr_mailbox_publish(m, id);
}

static void test_frame_loop(void) {
    fake_reset();
    NkXrFns f = fake_table();
    NkXrRun run;
    NkXrFrame fr;
    NkXrMailbox m;
    uint64_t data[NK_XR_MAILBOX_SLOTS] = {0, 0, 0};
    nk_xr_mailbox_init(&m);
    nk_xr_run_init(&run);

    // A session that is not running: no call is made.
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_ERROR_SESSION_NOT_RUNNING);
    CHECK(fk.n_wait == 0 && fk.n_begin_frame == 0 && fk.n_end_frame == 0);
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, NULL, &fr) == XR_ERROR_VALIDATION_FAILURE);

    // READY, SYNCHRONIZED: frames with no layers, nothing published yet.
    nk_xr_session_next(&run, XR_SESSION_STATE_READY);
    nk_xr_session_next(&run, XR_SESSION_STATE_SYNCHRONIZED);
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_SUCCESS);
    CHECK(strcmp(fk.order, "WBE") == 0);  // wait, begin, end, in that order
    CHECK(fr.slot == -1 && fr.frame_id == 0 && fr.show == 0 && fr.layers_submitted == 0);
    CHECK(fk.last_end_layer_count == 0 && fk.last_end_layers == NULL);

    // VISIBLE with nothing published: still zero layers.
    nk_xr_session_next(&run, XR_SESSION_STATE_VISIBLE);
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_SUCCESS);
    CHECK(fr.slot == -1 && fr.show == 0 && fk.last_end_layer_count == 0);

    // The producer publishes once; VISIBLE now sees it. SYNCHRONIZED sees the slot but shows nothing.
    ProducerLog pl = {0, 0};
    produce(&m, &pl, data, 5, -1);
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_SUCCESS);
    CHECK(fr.slot >= 0 && fr.frame_id == 5 && data[fr.slot] == 5 && fr.show == 1);
    CHECK(fr.layers_submitted == 0 && fk.last_end_layer_count == 0);
    fk.should_render = XR_FALSE;
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_SUCCESS);
    CHECK(fr.slot >= 0 && fr.show == 0);  // shouldRender false: nothing to show
    fk.should_render = XR_TRUE;
    nk_xr_session_next(&run, XR_SESSION_STATE_SYNCHRONIZED);
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_SUCCESS);
    CHECK(fr.slot >= 0 && fr.show == 0 && fk.last_end_layer_count == 0);

    // Failures stop the pass at the failing call.
    fake_reset();
    fk.wait_result = XR_ERROR_TIME_INVALID;
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_ERROR_TIME_INVALID);
    CHECK(strcmp(fk.order, "W") == 0);
    fake_reset();
    fk.begin_frame_result = XR_ERROR_CALL_ORDER_INVALID;
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_ERROR_CALL_ORDER_INVALID);
    CHECK(strcmp(fk.order, "WB") == 0);
    fake_reset();
    fk.end_frame_result = XR_ERROR_LAYER_INVALID;
    CHECK(nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr) == XR_ERROR_LAYER_INVALID);
    CHECK(strcmp(fk.order, "WBE") == 0);

    // Decoupling. The producer publishes 1000 frames. In run A nobody runs the frame
    // loop. In run B the frame loop runs every 10 frames and every xrWaitFrame stalls
    // (the fake records a long wait). The producer makes the same number of calls and
    // is never refused a slot, never gets the consumer's slot, and the stalls only show
    // up in the fake's own counter.
    enum { FRAMES = 1000, EVERY = 10 };
    ProducerLog a = {0, 0};
    nk_xr_mailbox_init(&m);
    for (uint64_t i = 1; i <= FRAMES; i++) {
        produce(&m, &a, data, i, -1);
    }
    CHECK(a.calls == FRAMES && a.bad == 0);

    fake_reset();
    ProducerLog b = {0, 0};
    int consumer_slot = -1;
    int loop_passes = 0;
    int loop_ok = 1;
    nk_xr_mailbox_init(&m);
    for (uint64_t i = 1; i <= FRAMES; i++) {
        produce(&m, &b, data, i, consumer_slot);
        if (i % EVERY == 0) {
            XrResult r = nk_xr_frame_once(&f, FAKE_SESSION, &run, &m, &fr);
            loop_passes++;
            loop_ok &= r == XR_SUCCESS && fr.slot >= 0 && fr.frame_id == i && data[fr.slot] == i;
            consumer_slot = fr.slot;
        }
    }
    CHECK(b.calls == a.calls && b.bad == 0);
    CHECK(loop_passes == FRAMES / EVERY && loop_ok);
    CHECK(fk.n_wait == FRAMES / EVERY && fk.stall == (uint64_t)(FRAMES / EVERY) * 1000000u);
    CHECK(fk.n_begin_frame == fk.n_wait && fk.n_end_frame == fk.n_wait);
    CHECK(fk.bad_args == 0);
}

// ---------------------------------------------------------------------------
// Layout maths.

static void test_layout(void) {
    float w = 0, h = 0;
    CHECK(nk_xr_quad_extent(2.0f, &w, &h) == 0);
    CHECK(w == 2.0f);
    CHECK(h == 1.13333333f);  // 2.0 * 272 / 480
    CHECK(nk_xr_quad_extent(4.8f, &w, &h) == 0 && w == 4.8f);
    CHECK(fabsf(h - 4.8f * 272.0f / 480.0f) < 1e-6f);
    CHECK(fabsf(w / h - 480.0f / 272.0f) < 1e-5f);
    w = 1.0f;
    h = 2.0f;
    CHECK(nk_xr_quad_extent(0.0f, &w, &h) == -1 && w == 1.0f && h == 2.0f);
    CHECK(nk_xr_quad_extent(-1.0f, &w, &h) == -1);
    CHECK(nk_xr_quad_extent(INFINITY, &w, &h) == -1);
    CHECK(nk_xr_quad_extent(NAN, &w, &h) == -1);
    CHECK(nk_xr_quad_extent(2.0f, NULL, &h) == -1);

    float pos[3] = {9, 9, 9}, quat[4] = {9, 9, 9, 9};
    CHECK(nk_xr_quad_pose(2.5f, 1.25f, pos, quat) == 0);
    CHECK(pos[0] == 0.0f && pos[1] == 1.25f && pos[2] == -2.5f);
    CHECK(quat[0] == 0.0f && quat[1] == 0.0f && quat[2] == 0.0f && quat[3] == 1.0f);
    CHECK(nk_xr_quad_pose(0.0f, 1.0f, pos, quat) == -1);
    CHECK(nk_xr_quad_pose(2.0f, NAN, pos, quat) == -1);
    CHECK(nk_xr_quad_pose(2.0f, 1.0f, NULL, quat) == -1);
    CHECK(pos[2] == -2.5f);  // refused calls leave the outputs alone

    // x' = x + eye * s * (w - c), with exactly representable values.
    const float s = 0.5f, c = 4.0f;
    CHECK(nk_xr_eye_clip_x(1.0f, 4.0f, s, c, -1) == 1.0f);  // w == c: unchanged
    CHECK(nk_xr_eye_clip_x(1.0f, 4.0f, s, c, +1) == 1.0f);
    CHECK(nk_xr_eye_clip_x(-3.5f, c, 123.0f, c, +1) == -3.5f);  // any separation, still unchanged
    CHECK(nk_xr_eye_clip_x(1.0f, 6.0f, s, c, +1) == 2.0f);      // 1 + 0.5 * 2
    CHECK(nk_xr_eye_clip_x(1.0f, 6.0f, s, c, -1) == 0.0f);      // 1 - 0.5 * 2
    CHECK(nk_xr_eye_clip_x(1.0f, 2.0f, s, c, +1) == 0.0f);      // nearer than convergence: reversed
    CHECK(nk_xr_eye_clip_x(1.0f, 2.0f, s, c, -1) == 2.0f);
    CHECK(nk_xr_eye_clip_x(1.0f, 6.0f, s, c, 0) == 1.0f);       // mono
    // Non-finite inputs are refused the way the other layout functions refuse them: NaN.
    CHECK(isnan(nk_xr_eye_clip_x(INFINITY, 4.0f, s, c, 0)));
    CHECK(isnan(nk_xr_eye_clip_x(1.0f, INFINITY, s, c, +1)));
    CHECK(isnan(nk_xr_eye_clip_x(1.0f, 4.0f, INFINITY, c, -1)));
    CHECK(isnan(nk_xr_eye_clip_x(1.0f, 4.0f, s, INFINITY, +1)));
    CHECK(isnan(nk_xr_eye_clip_x(NAN, 4.0f, s, c, 0)));
    for (int i = 0; i < 16; i++) {
        float x = 0.25f * (float)(i - 8), wc = 0.5f * (float)i;
        float r = nk_xr_eye_clip_x(x, wc, s, c, +1), l = nk_xr_eye_clip_x(x, wc, s, c, -1);
        CHECK((r - x) == -(l - x));  // symmetric about the mono position
    }
}

int main(void) {
    test_refusals();
    test_api_version_fallback();
    test_blend_mode_choice();
    test_null_table_refusals();
    test_loss_then_exit();
    test_state_machine();
    test_mailbox_basic();
    test_mailbox_interleavings();
    test_mailbox_producer_never_waits();
    test_mailbox_two_threads();
    test_frame_loop();
    test_layout();
    if (g_failures != 0) {
        printf("xr_selftest: FAIL (%d of %d checks failed)\n", g_failures, g_checks);
        return 1;
    }
    printf("xr_selftest: PASS (%d checks)\n", g_checks);
    return 0;
}
