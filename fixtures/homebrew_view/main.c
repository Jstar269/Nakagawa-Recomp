/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The homebrew folder probe (#2143). A PSP boots homebrew as
 * ms0:/PSP/GAME/<folder>/EBOOT.PBP, and PSPSDK's start-up code makes that folder the
 * program's working folder, so a program opens its own files by relative path. This
 * program does exactly that: it reads DATA.TXT beside itself, tries MISSING.TXT, which
 * the folder does not hold, writes WRITTEN.TXT beside itself, and records what it saw
 * in ms0:/NAKAGAWA_TEST/HOMEBREW_VIEW.TXT, the result tools/test_player_package_route.py
 * checks after importing, building and running it. */

#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("NK_HOMEBREW_VIEW", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

int main(int argc, char *argv[]) {
    char data[64];
    int data_bytes = -1;
    int missing_opened = 0;
    memset(data, 0, sizeof(data));

    FILE *own = fopen("DATA.TXT", "rb");
    if (own) {
        data_bytes = (int)fread(data, 1, sizeof(data) - 1, own);
        fclose(own);
    }
    FILE *missing = fopen("MISSING.TXT", "rb");
    if (missing) {
        missing_opened = 1;
        fclose(missing);
    }
    FILE *written = fopen("WRITTEN.TXT", "wb");
    if (written) {
        fputs("written by the program", written);
        fclose(written);
    }

    sceIoMkdir("ms0:/NAKAGAWA_TEST", 0777);
    FILE *result = fopen("ms0:/NAKAGAWA_TEST/HOMEBREW_VIEW.TXT", "wb");
    if (result) {
        fprintf(result, "ARGV0=%s\nDATA=%s\nDATA_BYTES=%d\nMISSING_OPENED=%d\n",
                argc > 0 ? argv[0] : "", data, data_bytes, missing_opened);
        fclose(result);
    }
    sceKernelExitGame();
    return 0;
}
