/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * libnk_ld.so: the linker clang starts on the device (#360).
 *
 * An app may execute only files in its native library folder, and that folder holds only
 * names of the form lib<name>.so. lld picks its role from the name it was started under
 * (ld.lld, ld64.lld, lld-link, wasm-ld), so lld installed as libnk_lld.so refuses to run:
 * "lld is a generic driver". This launcher sits next to it, is what clang's --ld-path
 * names, and starts libnk_lld.so from the same folder with argv[0] set to "ld.lld". Every
 * other argument passes through unchanged, and lld's exit status is the launcher's.
 *
 * It returns 127 when its own path cannot be read or libnk_lld.so cannot be started, as a
 * shell does for a command it cannot run.
 *
 * It is built only for Android (and Linux, for its test); like src/core/nk_platform_posix.c,
 * a Windows compile sees an empty unit.
 */

#if !defined(_WIN32) && !defined(_WIN64)

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NK_LD_TARGET "libnk_lld.so"
#define NK_LD_ROLE "ld.lld"
#define NK_LD_CANNOT_RUN 127

int main(int argc, char **argv)
{
    char self[PATH_MAX];
    char target[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length <= 0) {
        fprintf(stderr, "libnk_ld: cannot read /proc/self/exe: %s\n", strerror(errno));
        return NK_LD_CANNOT_RUN;
    }
    self[length] = '\0';
    char *slash = strrchr(self, '/');
    if (slash == NULL) {
        fprintf(stderr, "libnk_ld: %s is not an absolute path\n", self);
        return NK_LD_CANNOT_RUN;
    }
    *slash = '\0';
    int written = snprintf(target, sizeof(target), "%s/%s", self, NK_LD_TARGET);
    if (written < 0 || (size_t)written >= sizeof(target)) {
        fprintf(stderr, "libnk_ld: the path of %s in %s is too long\n", NK_LD_TARGET, self);
        return NK_LD_CANNOT_RUN;
    }
    if (argc < 1) {
        fprintf(stderr, "libnk_ld: started without argv[0]\n");
        return NK_LD_CANNOT_RUN;
    }
    argv[0] = (char *)NK_LD_ROLE;
    execv(target, argv);
    fprintf(stderr, "libnk_ld: cannot start %s: %s\n", target, strerror(errno));
    return NK_LD_CANNOT_RUN;
}

#endif /* !_WIN32 */
