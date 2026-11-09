// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Helper module for CASE=hle-self-stop-unload (PSP-HLE-SELFSTOP-001, probe_hle_measure.c).
// CASE=hle-self-stop-helper builds this PRX; it is not a probe, never writes a record and is
// only loaded by that case from host0:/hle_self_stop_helper.prx.
//
// The probe starts the module with three argument strings (argv[0] is the module path, as
// for any module): argv[1] is the address of the probe's 8-word marker buffer in hex and
// argv[2] the mode. The module's main thread (the thread the SDK start-up code creates, which
// is the module's own thread) then:
//   1. writes the entered marker and the address of an anchor function inside this module;
//   2. calls sceKernelSelfStopUnloadModule with the arguments of the mode;
//   3. if the call returns, writes its return value and a continued marker, then exits the
//      thread (it never returns from main, which would end the whole program).
// module_stop returns 0 and writes the stop marker, the argument count and the argument
// pointer it was given, and a call count, into the same buffer. Nothing else is touched.

#include <pspkernel.h>
#include <stddef.h>
#include <stdint.h>

PSP_MODULE_INFO("PSP_HLE_SELFSTOP", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16);

#define HS_W_ENTERED 0u
#define HS_W_ANCHOR 1u
#define HS_W_STOP 2u
#define HS_W_STOP_ARGS 3u
#define HS_W_STOP_ARGP 4u
#define HS_W_RETURN 5u
#define HS_W_CONTINUED 6u
#define HS_W_STOP_COUNT 7u

#define HS_ENTERED_MARK 0x5E1F0001u
#define HS_CONTINUED_MARK 0x5E1F0002u
#define HS_STOP_MARK 0x5E1F0003u

/* Modes: 0 = (0, 0, NULL); 1 = (1, 0, NULL); 2 = (0, 4, an address past RAM). */
#define HS_PAST_RAM_ADDRESS 0x0C000000u

static volatile uint32_t *s_words;

static uint32_t hs_parse(const char *text, uint32_t base)
{
    uint32_t value = 0;
    if (base == 16 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    for (; *text != '\0'; text++) {
        uint32_t digit;
        if (*text >= '0' && *text <= '9') {
            digit = (uint32_t)(*text - '0');
        } else if (*text >= 'a' && *text <= 'f') {
            digit = (uint32_t)(*text - 'a') + 10u;
        } else if (*text >= 'A' && *text <= 'F') {
            digit = (uint32_t)(*text - 'A') + 10u;
        } else {
            break;
        }
        value = value * base + digit;
    }
    return value;
}

/* An address inside this module for sceKernelGetModuleIdByAddress. */
__attribute__((noinline, used)) static int hs_anchor(void)
{
    return 0x5E1F;
}

int module_stop(SceSize args, void *argp)
{
    volatile uint32_t *words = s_words;
    if (words != NULL) {
        words[HS_W_STOP_COUNT] = words[HS_W_STOP_COUNT] + 1u;
        words[HS_W_STOP_ARGS] = (uint32_t)args;
        words[HS_W_STOP_ARGP] = (uint32_t)(uintptr_t)argp;
        words[HS_W_STOP] = HS_STOP_MARK;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        sceKernelExitThread(0);
        return 0;
    }
    volatile uint32_t *words = (volatile uint32_t *)(uintptr_t)hs_parse(argv[1], 16);
    const uint32_t mode = hs_parse(argv[2], 10);
    s_words = words;
    words[HS_W_ANCHOR] = (uint32_t)(uintptr_t)&hs_anchor;
    words[HS_W_ENTERED] = HS_ENTERED_MARK;

    int rc;
    if (mode == 1u) {
        rc = sceKernelSelfStopUnloadModule(1, 0, NULL);
    } else if (mode == 2u) {
        rc = sceKernelSelfStopUnloadModule(0, 4, (void *)(uintptr_t)HS_PAST_RAM_ADDRESS);
    } else {
        rc = sceKernelSelfStopUnloadModule(0, 0, NULL);
    }
    words[HS_W_RETURN] = (uint32_t)rc;
    words[HS_W_CONTINUED] = HS_CONTINUED_MARK;
    sceKernelExitThread(0);
    return 0;
}
