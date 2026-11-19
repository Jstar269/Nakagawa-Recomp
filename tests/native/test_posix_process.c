/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* POSIX process backend: wait timeouts and exit-status reporting.
 *
 * nk_platform.h reserves a negative timeout for an infinite wait, and the
 * Win32 backend has always honoured finite ones through WaitForSingleObject.
 * The POSIX backend discarded the value and always blocked, so a caller such
 * as nk_launch_wait(session, 5000) hung forever whenever the runtime stalled.
 * Win32 has tests/native/test_win32_process.c; this is the equivalent for the
 * other backend, and only builds where that backend does.
 *
 * The children are this program itself in a child mode (--child-sleep, --child-exit,
 * --child-tree), so the same subtests run wherever the backend does. On Android the app may
 * run only the programs its APK ships, from the folder its code was loaded from: there they
 * start through nk_platform_spawn_tool, which a test program run from its own folder
 * satisfies, and a program anywhere else must be refused (#360).
 */

/* Scratch root for the files this test writes: the checkout's build/ by default, or
 * the BUILD_ROOT the Makefile passes as -DSR_SELFTEST_BUILD_ROOT, so a scratch run never
 * touches the checkout. */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#if defined(_WIN32) || defined(_WIN64)

#include <stdio.h>

int main(void) {
    printf("[POSIX_PROCESS_TEST] Skipped: this backend is not built on Windows.\n");
    return 0;
}

#else

/* Strict -std=c99 hides clock_gettime, nanosleep and CLOCK_MONOTONIC behind
   the feature-test macro, exactly as src/core/nk_platform_posix.c does; symlink and
   mkdtemp need the XSI level. */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "nk_launch.h"
#include "nk_platform.h"

/* The checks below spawn and wait inside assert(): keep them in every build. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char s_self[NK_MAX_PATH];

static double elapsed_ms(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0
         + (double)(b->tv_nsec - a->tv_nsec) / 1000000.0;
}

static void pause_ms(long ms) {
    struct timespec slice = { ms / 1000L, (ms % 1000L) * 1000L * 1000L };
    nanosleep(&slice, NULL);
}

/* The child modes. --child-tree starts a grandchild that sleeps, records its pid in the named
 * file and waits for it, as `sh -c "sleep 60 & echo $! > file; wait"` would. */
static int child_main(int argc, char **argv) {
    if (strcmp(argv[1], "--child-sleep") == 0 && argc >= 3) {
        pause_ms(atol(argv[2]) * 1000L);
        return 0;
    }
    if (strcmp(argv[1], "--child-exit") == 0 && argc >= 3) return atoi(argv[2]);
    if (strcmp(argv[1], "--child-tree") == 0 && argc >= 3) {
        pid_t grandchild = fork();
        if (grandchild < 0) return 98;
        if (grandchild == 0) {
            pause_ms(60000L);
            _exit(0);
        }
        FILE *pf = fopen(argv[2], "w");
        if (!pf) return 97;
        fprintf(pf, "%d\n", (int)grandchild);
        fclose(pf);
        int status = 0;
        (void)waitpid(grandchild, &status, 0);
        return 0;
    }
    /* --child-nice writes the niceness a grandchild of the spawned child runs at. */
    if (strcmp(argv[1], "--child-nice") == 0 && argc >= 3) {
        pid_t grandchild = fork();
        if (grandchild < 0) return 98;
        if (grandchild == 0) {
            FILE *nf = fopen(argv[2], "w");
            if (!nf) _exit(97);
            errno = 0;
            int niceness = getpriority(PRIO_PROCESS, 0);
            fprintf(nf, "%d\n", niceness == -1 && errno != 0 ? -100 : niceness);
            fclose(nf);
            _exit(0);
        }
        int status = 0;
        if (waitpid(grandchild, &status, 0) != grandchild) return 96;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 95;
    }
    /* --child-mask reports what the program started with: 1 when SIGTERM is blocked, 2 when
       SIGPIPE is ignored, 0 when neither. */
    if (strcmp(argv[1], "--child-mask") == 0) {
        sigset_t blocked;
        if (sigprocmask(SIG_BLOCK, NULL, &blocked) != 0) return 94;
        if (sigismember(&blocked, SIGTERM)) return 1;
        struct sigaction pipe_action;
        if (sigaction(SIGPIPE, NULL, &pipe_action) != 0) return 93;
        if (pipe_action.sa_handler == SIG_IGN) return 2;
        return 0;
    }
    return 99;
}

/* Starts this program in a child mode the way the backend under test starts a program here. */
static bool spawn_self(const char *mode, const char *value, NkProcessHandle *out) {
    const char *argv[] = { s_self, mode, value, NULL };
#if defined(__ANDROID__)
    return nk_platform_spawn_tool(s_self, argv, NULL, NULL, NULL, out);
#else
    return nk_platform_spawn_process(s_self, argv, NULL, NULL, NULL, out);
#endif
}

/* 0. Settings > Build speed: Background starts the build child at nice
 * NK_PLATFORM_LOW_PRIORITY_NICE, and what the child starts inherits it (the build's Make and
 * compilers). Before nk_platform_spawn_process_at_priority every child ran at the player's
 * priority. On Android the build is a tool child, so a low one starts through
 * nk_platform_spawn_tool_at_priority, which keeps the tool-folder rule at every priority. */
static bool spawn_self_low(const char *mode, const char *value, NkProcessHandle *out) {
    const char *argv[] = { s_self, mode, value, NULL };
#if defined(__ANDROID__)
    return nk_platform_spawn_tool_at_priority(s_self, argv, NULL, NULL, NULL,
                                              NK_PROCESS_PRIORITY_LOW, out);
#else
    return nk_platform_spawn_process_at_priority(s_self, argv, NULL, NULL, NULL,
                                                 NK_PROCESS_PRIORITY_LOW, out);
#endif
}

static void low_priority_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 0: a low-priority child and its children\n");
    fflush(stdout);
    NkProcessPriority read_back = NK_PROCESS_PRIORITY_NORMAL;
    NkProcessHandle low;
    assert(spawn_self_low("--child-sleep", "30", &low));
    assert(nk_platform_process_priority(&low, &read_back));
    assert(read_back == NK_PROCESS_PRIORITY_LOW);
    nk_platform_terminate_process(&low);
    nk_platform_close_process(&low);

    const char *nice_file = SR_SELFTEST_BUILD_ROOT "/posix_process_nice.txt";
    (void)remove(nice_file);
    NkProcessHandle tree;
    assert(spawn_self_low("--child-nice", nice_file, &tree));
    assert(nk_platform_wait_process(&tree, 10000) == 0);
    nk_platform_close_process(&tree);
    FILE *nf = fopen(nice_file, "r");
    int niceness = -1;
    assert(nf != NULL);
    assert(fscanf(nf, "%d", &niceness) == 1);
    fclose(nf);
    (void)remove(nice_file);
    assert(niceness >= NK_PLATFORM_LOW_PRIORITY_NICE);

#if defined(__ANDROID__)
    /* A low priority is no way around the tool-folder rule. */
    NkProcessHandle none;
    const char *outside_argv[] = { "/system/bin/true", NULL };
    assert(!nk_platform_spawn_tool_at_priority("/system/bin/true", outside_argv, NULL, NULL, NULL,
                                               NK_PROCESS_PRIORITY_LOW, &none));
    assert(none.spawn_error == EACCES);
#endif
}

/* 8. nk_platform_get_file_size POSIX file size queries. */
static void file_size_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 8: nk_platform_get_file_size file size queries\n");
    fflush(stdout);

    /* 8a. NULL path handling returns -1 */
    assert(nk_platform_get_file_size(NULL) == -1);

    /* 8b. Non-existent file path returns -1 */
    const char *nonexistent_path = SR_SELFTEST_BUILD_ROOT "/test_posix_nonexistent_12345.tmp";
    remove(nonexistent_path);
    assert(nk_platform_get_file_size(nonexistent_path) == -1);

    /* 8c. Empty file returns 0 */
    const char *empty_path = SR_SELFTEST_BUILD_ROOT "/test_posix_empty.tmp";
    FILE *f_empty = fopen(empty_path, "wb");
    assert(f_empty != NULL);
    fclose(f_empty);
    assert(nk_platform_get_file_size(empty_path) == 0);
    remove(empty_path);

    /* 8d. Regular file with known size */
    const char *data_path = SR_SELFTEST_BUILD_ROOT "/test_posix_data.tmp";
    FILE *f_data = fopen(data_path, "wb");
    assert(f_data != NULL);
    char test_buf[2048];
    memset(test_buf, 0x5A, sizeof(test_buf));
    assert(fwrite(test_buf, 1, sizeof(test_buf), f_data) == sizeof(test_buf));
    fclose(f_data);
    assert(nk_platform_get_file_size(data_path) == (int64_t)sizeof(test_buf));
    remove(data_path);
}

static void touch(const char *path) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fclose(f);
}

/* 9. The tool-folder rule nk_platform_spawn_tool applies on Android: a program qualifies only
 * when it resolves, links followed, to a regular file directly in the folder. */
static void tool_dir_rule_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 9: the bundled-tool folder rule\n");
    fflush(stdout);
    /* Inside the run's scratch BUILD_ROOT, never the host's shared TMPDIR, so a failed run
       leaves its tree only there. Absolute, so the links below resolve from any folder. */
    char root[NK_MAX_PATH];
    char *scratch = realpath(SR_SELFTEST_BUILD_ROOT, NULL);
    assert(scratch != NULL);
    int root_len = snprintf(root, sizeof(root), "%s/nk-tool-rule-XXXXXX", scratch);
    free(scratch);
    assert(root_len > 0 && (size_t)root_len < sizeof(root));
    assert(mkdtemp(root) != NULL);
    char tools[NK_MAX_PATH + 8], other[NK_MAX_PATH + 8], nested[NK_MAX_PATH + 8];
    char tool[NK_MAX_PATH + 32], outside[NK_MAX_PATH + 32], deep[NK_MAX_PATH + 32];
    char link_out[NK_MAX_PATH + 32], link_in[NK_MAX_PATH + 32], escape[NK_MAX_PATH + 32];
    char missing[NK_MAX_PATH + 32];
    snprintf(tools, sizeof(tools), "%s/lib", root);
    snprintf(other, sizeof(other), "%s/files", root);
    snprintf(nested, sizeof(nested), "%s/lib/sub", root);
    assert(mkdir(tools, 0700) == 0 && mkdir(other, 0700) == 0 && mkdir(nested, 0700) == 0);
    snprintf(tool, sizeof(tool), "%s/libtool.so", tools);
    snprintf(outside, sizeof(outside), "%s/written", other);
    snprintf(deep, sizeof(deep), "%s/libdeep.so", nested);
    snprintf(link_out, sizeof(link_out), "%s/liblink.so", tools);
    snprintf(link_in, sizeof(link_in), "%s/link-to-tool", other);
    snprintf(escape, sizeof(escape), "%s/../files/written", tools);
    snprintf(missing, sizeof(missing), "%s/libmissing.so", tools);
    touch(tool);
    touch(outside);
    touch(deep);
    assert(symlink(outside, link_out) == 0);
    assert(symlink(tool, link_in) == 0);

    assert(nk_platform_path_is_tool_in_dir(tools, tool));
    assert(nk_platform_path_is_tool_in_dir(tools, link_in));     /* resolves into the folder */
    assert(!nk_platform_path_is_tool_in_dir(tools, outside));
    assert(!nk_platform_path_is_tool_in_dir(tools, escape));     /* ".." out of the folder */
    assert(!nk_platform_path_is_tool_in_dir(tools, link_out));   /* resolves out of it */
    assert(!nk_platform_path_is_tool_in_dir(tools, deep));       /* below it, not in it */
    assert(!nk_platform_path_is_tool_in_dir(tools, nested));     /* a folder, not a file */
    assert(!nk_platform_path_is_tool_in_dir(tools, tools));
    assert(!nk_platform_path_is_tool_in_dir(tools, missing));
    assert(!nk_platform_path_is_tool_in_dir(NULL, tool));
    assert(!nk_platform_path_is_tool_in_dir(tools, ""));

    remove(link_in);
    remove(link_out);
    remove(deep);
    remove(outside);
    remove(tool);
    rmdir(nested);
    rmdir(other);
    rmdir(tools);
    rmdir(root);
}

#if defined(__ANDROID__)
/* A title starts through TitleActivity over JNI, and a program run from adb shell has no Java
 * VM, so the title spawn must fail closed and leave no handle a caller could wait on. A tool
 * outside the tool folder must be refused before anything runs. */
static void android_refusals_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 10: Android refuses what it may not start\n");
    fflush(stdout);
    NkProcessHandle none;
    memset(&none, 0x5a, sizeof(none));
    const char *argv[] = { "/system/bin/true", NULL };
    assert(!nk_platform_spawn_process("/system/bin/true", argv, NULL, NULL, NULL, &none));
    assert(none.spawn_error == ENOSYS);
    assert(!none.is_active && none.native_handle == NULL && none.process_id == 0);

    memset(&none, 0x5a, sizeof(none));
    assert(!nk_platform_spawn_tool("/system/bin/true", argv, NULL, NULL, NULL, &none));
    assert(none.spawn_error == EACCES);
    assert(!none.is_active && !none.tool_child && none.process_id == 0);

    char tool_dir[NK_MAX_PATH];
    assert(nk_platform_get_tool_dir(tool_dir, sizeof(tool_dir)));
    assert(nk_platform_path_is_tool_in_dir(tool_dir, s_self));
}
#else
/* On the desktop a tool program is an ordinary child. */
static void desktop_tool_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 10: a desktop tool is an ordinary child\n");
    fflush(stdout);
    char unused[8];
    assert(!nk_platform_get_tool_dir(unused, sizeof(unused)));
    NkProcessHandle tool;
    memset(&tool, 0, sizeof(tool));
    const char *argv[] = { s_self, "--child-exit", "4", NULL };
    assert(nk_platform_spawn_tool(s_self, argv, NULL, NULL, NULL, &tool));
    assert(!tool.tool_child);
    assert(nk_platform_wait_process(&tool, 10000) == 4);
    nk_platform_close_process(&tool);
}
#endif

/* nk_platform_env_utf8 gives the bytes as they are: unset is 0, set and empty is 1, a value that
   does not fit is -1, with `out` emptied. */
static void env_utf8_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 11: nk_platform_env_utf8\n");
    fflush(stdout);
    const char *expected = "pack_\xC3\xA9\xE6\xBC\xA2";    /* 10 bytes */
    char out[64];
    assert(unsetenv("NK_ENV_UTF8_TEST") == 0);
    out[0] = 'x';
    assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, sizeof(out)) == 0 && out[0] == '\0');
    assert(setenv("NK_ENV_UTF8_TEST", "", 1) == 0);
    out[0] = 'x';
    assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, sizeof(out)) == 1 && out[0] == '\0');
    assert(setenv("NK_ENV_UTF8_TEST", expected, 1) == 0);
    assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, sizeof(out)) == 1 && strcmp(out, expected) == 0);
    assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, 11) == 1 && strcmp(out, expected) == 0);
    out[0] = 'x';
    assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, 10) == -1 && out[0] == '\0');
    assert(unsetenv("NK_ENV_UTF8_TEST") == 0);
}

static void scratch_root(char *root, size_t capacity, const char *name) {
    snprintf(root, capacity, "%s/%s-XXXXXX",
             getenv("TMPDIR") && getenv("TMPDIR")[0] ? getenv("TMPDIR") : "/tmp", name);
    assert(mkdtemp(root) != NULL);
}

/* 12. nk_platform_mkdir_p_private follows a symlink at an ancestor that already exists
 * (/home -> var/home on Fedora Silverblue, /tmp -> private/tmp on macOS) and still refuses a
 * symlink as the final folder. It used to lstat every prefix, so any data path under a
 * symlinked folder failed and every library lock with it. */
static void mkdir_private_symlink_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 12: a symlinked ancestor of a private folder\n");
    fflush(stdout);
    char root[NK_MAX_PATH];
    scratch_root(root, sizeof(root), "nk-mkdir-private");
    char real[NK_MAX_PATH + 16], link[NK_MAX_PATH + 16], below[NK_MAX_PATH + 32];
    char leaf[NK_MAX_PATH + 32], mid[NK_MAX_PATH + 32];
    snprintf(real, sizeof(real), "%s/real", root);
    snprintf(link, sizeof(link), "%s/link", root);
    snprintf(below, sizeof(below), "%s/link/a/b", root);
    snprintf(mid, sizeof(mid), "%s/real/a", root);
    snprintf(leaf, sizeof(leaf), "%s/real/a/b", root);
    assert(mkdir(real, 0700) == 0);
    assert(symlink("real", link) == 0);

    assert(nk_platform_mkdir_p_private(below));
    struct stat st;
    assert(lstat(leaf, &st) == 0 && S_ISDIR(st.st_mode));
    assert((st.st_mode & 0777) == 0700);
    assert(!nk_platform_mkdir_p_private(link));          /* a final symlink stays refused */

    rmdir(leaf);
    rmdir(mid);
    remove(link);
    rmdir(real);
    rmdir(root);
}

/* 13. The per-user folders nk_platform_get_path creates are owner-only. They were made
 * 0755, so under umask 022 other local users could list and read saves and logs. */
static void get_path_private_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 13: per-user folders are created owner-only\n");
    fflush(stdout);
    char root[NK_MAX_PATH];
    scratch_root(root, sizeof(root), "nk-get-path");
    char data_root[NK_MAX_PATH + 16];
    snprintf(data_root, sizeof(data_root), "%s/data", root);
    mode_t old_mask = umask(022);
    assert(nk_platform_set_app_data_dir_override(data_root));
    char saves[NK_MAX_PATH];
    assert(nk_platform_get_path(NK_PATH_SAVES, saves, sizeof(saves)));
    assert(nk_platform_set_app_data_dir_override(NULL));
    umask(old_mask);
    struct stat st;
    assert(stat(data_root, &st) == 0 && S_ISDIR(st.st_mode));
    assert((st.st_mode & 0777) == 0700);
    assert(stat(saves, &st) == 0 && S_ISDIR(st.st_mode));
    assert((st.st_mode & 0777) == 0700);
    assert(nk_platform_remove_tree(root, NULL));
}

struct listed_names {
    int hidden;
    int shown;
    int other;
};

static bool count_listed_name(const char *name, void *ctx) {
    struct listed_names *seen = (struct listed_names *)ctx;
    if (strcmp(name, ".hidden.json") == 0) seen->hidden++;
    else if (strcmp(name, "shown.json") == 0) seen->shown++;
    else seen->other++;
    return true;
}

/* 14. nk_platform_list_files lists a file whose name starts with a dot, as the Win32 backend
 * does and as nk_platform.h says ("each regular file directly inside dir"). POSIX dropped
 * every such name, so a caller saw different files on the two hosts. */
static void list_files_dot_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 14: list_files lists dot-files\n");
    fflush(stdout);
    char root[NK_MAX_PATH];
    scratch_root(root, sizeof(root), "nk-list-files");
    char hidden[NK_MAX_PATH + 32], shown[NK_MAX_PATH + 32], sub[NK_MAX_PATH + 32];
    snprintf(hidden, sizeof(hidden), "%s/.hidden.json", root);
    snprintf(shown, sizeof(shown), "%s/shown.json", root);
    snprintf(sub, sizeof(sub), "%s/.sub", root);
    touch(hidden);
    touch(shown);
    assert(mkdir(sub, 0700) == 0);
    struct listed_names seen = { 0, 0, 0 };
    assert(nk_platform_list_files(root, count_listed_name, &seen));
    assert(seen.hidden == 1 && seen.shown == 1 && seen.other == 0);
    rmdir(sub);
    remove(shown);
    remove(hidden);
    rmdir(root);
}

/* 15. A child starts with no signal blocked and SIGPIPE at its default, whatever the spawning
 * thread had. SDL's threads block SIGTERM and the player starts games from one, so the child
 * kept SIGTERM blocked and every Stop waited out the grace period and ended in SIGKILL. */
static void child_signal_state_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 15: a child does not inherit blocked signals\n");
    fflush(stdout);
    sigset_t block, old_set;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    assert(sigprocmask(SIG_BLOCK, &block, &old_set) == 0);
    struct sigaction ignore, old_pipe;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    assert(sigaction(SIGPIPE, &ignore, &old_pipe) == 0);

    NkProcessHandle child;
    memset(&child, 0, sizeof(child));
    bool started = spawn_self("--child-mask", "", &child);
    assert(sigaction(SIGPIPE, &old_pipe, NULL) == 0);
    assert(sigprocmask(SIG_SETMASK, &old_set, NULL) == 0);
    assert(started);
    assert(nk_platform_wait_process(&child, 10000) == 0);
    nk_platform_close_process(&child);
}

#if !defined(__ANDROID__)
/* 16. A program that exists but cannot be executed fails the spawn with exec's errno. The
 * spawn reported success with spawn_error 0, and the caller only later saw exit code 127. */
static void exec_failure_subtest(void) {
    printf("[POSIX_PROCESS_TEST] Subtest 16: a failed exec fails the spawn with its errno\n");
    fflush(stdout);
    char root[NK_MAX_PATH];
    scratch_root(root, sizeof(root), "nk-exec-fail");
    char plain[NK_MAX_PATH + 32], missing[NK_MAX_PATH + 32];
    snprintf(plain, sizeof(plain), "%s/not-executable", root);
    snprintf(missing, sizeof(missing), "%s/missing", root);
    touch(plain);
    assert(chmod(plain, 0600) == 0);

    NkProcessHandle child;
    memset(&child, 0, sizeof(child));
    const char *argv_plain[] = { plain, NULL };
    assert(!nk_platform_spawn_process(plain, argv_plain, NULL, NULL, NULL, &child));
    assert(child.spawn_error == EACCES);
    assert(!child.is_active && child.process_id == 0);

    memset(&child, 0, sizeof(child));
    const char *argv_missing[] = { missing, NULL };
    assert(!nk_platform_spawn_process(missing, argv_missing, NULL, NULL, NULL, &child));
    assert(child.spawn_error == ENOENT);
    assert(!child.is_active && child.process_id == 0);

    /* A working folder that cannot be entered fails the same way, before exec. */
    memset(&child, 0, sizeof(child));
    const char *argv_self[] = { s_self, "--child-exit", "0", NULL };
    assert(!nk_platform_spawn_process(s_self, argv_self, NULL, missing, NULL, &child));
    assert(child.spawn_error == ENOENT);

    remove(plain);
    rmdir(root);
}
#endif

int main(int argc, char **argv) {
    if (argc >= 2 && strncmp(argv[1], "--child-", 8) == 0) return child_main(argc, argv);
    assert(nk_platform_get_executable_path(s_self, sizeof(s_self)));

    struct timespec t0, t1;

    low_priority_subtest();

    /* 1. A finite timeout must expire and return, leaving the child alive.
     *
     * Before the fix this blocked for the child's full 30 seconds and then
     * returned sleep's exit status, so both assertions below failed. */
    printf("[POSIX_PROCESS_TEST] Subtest 1: a finite timeout expires\n");
    fflush(stdout);

    NkProcessHandle slow;
    memset(&slow, 0, sizeof(slow));
    assert(spawn_self("--child-sleep", "30", &slow));

    assert(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
    int timed_out = nk_platform_wait_process(&slow, 300);
    assert(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);

    assert(timed_out == -1);
    assert(elapsed_ms(&t0, &t1) < 5000.0);
    /* The handle is still usable: a timeout is not a reap. */
    assert(nk_platform_is_process_running(&slow));

    nk_platform_terminate_process(&slow);
    nk_platform_close_process(&slow);

    /* 2. A child that finishes inside the timeout reports its real code. */
    printf("[POSIX_PROCESS_TEST] Subtest 2: exit code within the timeout\n");
    fflush(stdout);

    NkProcessHandle quick;
    memset(&quick, 0, sizeof(quick));
    assert(spawn_self("--child-exit", "7", &quick));
    assert(nk_platform_wait_process(&quick, 10000) == 7);
    nk_platform_close_process(&quick);

    /* 3. A negative timeout still means an unbounded wait. */
    printf("[POSIX_PROCESS_TEST] Subtest 3: negative timeout waits\n");
    fflush(stdout);

    NkProcessHandle blocking;
    memset(&blocking, 0, sizeof(blocking));
    assert(spawn_self("--child-exit", "5", &blocking));
    assert(nk_platform_wait_process(&blocking, -1) == 5);
    nk_platform_close_process(&blocking);

    /* 4. A child already reaped by the running-check still reports its code.
     *
     * A POSIX child can be reaped exactly once. nk_platform_is_process_running
     * reaps it to learn that it exited, which used to throw the status away and
     * leave nk_platform_wait_process with nothing but ECHILD -- so the player,
     * which polls before waiting, printed "exited with code -1" for every run
     * on POSIX regardless of what the runtime actually returned. */
    printf("[POSIX_PROCESS_TEST] Subtest 4: status survives the running-check\n");
    fflush(stdout);

    NkProcessHandle polled;
    memset(&polled, 0, sizeof(polled));
    assert(spawn_self("--child-exit", "3", &polled));

    bool observed_exit = false;
    for (int i = 0; i < 2000; i++) {
        if (!nk_platform_is_process_running(&polled)) {
            observed_exit = true;
            break;
        }
        pause_ms(5);
    }
    assert(observed_exit);
    assert(nk_platform_wait_process(&polled, 0) == 3);
    nk_platform_close_process(&polled);

    /* 5. The nk_launch wrapper must keep a timed-out session usable.
     *
     * Both backends leave the handle valid when a finite wait expires, but the
     * wrapper recorded -1 as the exit code and marked the session stopped. A
     * caller that timed out could then neither wait again nor stop the child:
     * nk_launch_stop skips termination when is_running is false, and went on to
     * discard a still-live handle. The session is built directly here because
     * nk_launch_start would insist on a prepared runtime; the wrapper under
     * test does not care how the handle was filled in. */
    printf("[POSIX_PROCESS_TEST] Subtest 5: a timed-out session stays usable\n");
    fflush(stdout);

    NkLaunchSession session;
    memset(&session, 0, sizeof(session));
    assert(spawn_self("--child-sleep", "30", &session.process));
    session.is_running = true;
    pid_t session_pid = (pid_t)session.process.process_id;

    assert(nk_launch_wait(&session, 200) == -1);
    assert(session.is_running);                 /* still the caller's to stop */
    assert(nk_launch_is_running(&session));

    nk_launch_stop(&session);
    assert(!session.is_running);
    /* Stop must reap, not just signal: an unreaped child stays a zombie until
     * the player exits, and kill(pid, 0) still succeeds for one. */
    assert(kill(session_pid, 0) != 0);

    /* 6. A session whose exit was observed by polling reports the real code.
     *
     * This is the sequence src/player/main.c uses -- is_running first, then
     * wait -- and it reported 0 for every runtime exit on POSIX. */
    printf("[POSIX_PROCESS_TEST] Subtest 6: a polled exit keeps its code\n");
    fflush(stdout);

    memset(&session, 0, sizeof(session));
    assert(spawn_self("--child-exit", "9", &session.process));
    session.is_running = true;

    bool session_exited = false;
    for (int i = 0; i < 2000; i++) {
        if (!nk_launch_is_running(&session)) {
            session_exited = true;
            break;
        }
        pause_ms(5);
    }
    assert(session_exited);
    assert(nk_launch_wait(&session, 0) == 9);
    assert(session.exit_code == 9);
    nk_launch_stop(&session);

    /* 7. Terminating a child process must terminate its process tree (grandchildren).
     *
     * Before the fix, nk_platform_terminate_process signaled only the direct
     * child, leaving background children/grandchildren running until they
     * finished on their own. */
    printf("[POSIX_PROCESS_TEST] Subtest 7: process tree termination stops grandchildren\n");
    fflush(stdout);

    const char *posix_pid_file = SR_SELFTEST_BUILD_ROOT "/test_posix_grandchild.pid";
    char pid_file_abs[NK_MAX_PATH];
    remove(posix_pid_file);
    /* The child resolves the file name itself; give it the absolute form. */
    assert(getcwd(pid_file_abs, sizeof(pid_file_abs)) != NULL);
    if (posix_pid_file[0] == '/') {
        snprintf(pid_file_abs, sizeof(pid_file_abs), "%s", posix_pid_file);
    } else {
        size_t used = strlen(pid_file_abs);
        snprintf(pid_file_abs + used, sizeof(pid_file_abs) - used, "/%s", posix_pid_file);
    }

    NkProcessHandle tree_proc;
    memset(&tree_proc, 0, sizeof(tree_proc));
    assert(spawn_self("--child-tree", pid_file_abs, &tree_proc));
    assert(tree_proc.is_active);

    pid_t grandchild_pid = 0;
    for (int i = 0; i < 200; i++) {
        FILE *pf = fopen(posix_pid_file, "r");
        if (pf) {
            int pid_val = 0;
            if (fscanf(pf, "%d", &pid_val) == 1 && pid_val > 0) {
                grandchild_pid = (pid_t)pid_val;
                fclose(pf);
                break;
            }
            fclose(pf);
        }
        pause_ms(10);
    }
    assert(grandchild_pid > 0);
    assert(kill(grandchild_pid, 0) == 0);

    nk_platform_terminate_process(&tree_proc);
    nk_platform_close_process(&tree_proc);

    bool grandchild_dead = false;
    for (int i = 0; i < 200; i++) {
        if (kill(grandchild_pid, 0) != 0 && errno == ESRCH) {
            grandchild_dead = true;
            break;
        }
        pause_ms(10);
    }
    if (!grandchild_dead) {
        kill(grandchild_pid, SIGKILL);
        remove(posix_pid_file);
        fprintf(stderr, "[POSIX_PROCESS_TEST] Grandchild PID %d survived child termination!\n", (int)grandchild_pid);
        assert(grandchild_dead);
    }
    remove(posix_pid_file);
    printf("[POSIX_PROCESS_TEST] Grandchild PID %d terminated successfully.\n", (int)grandchild_pid);

    file_size_subtest();
    tool_dir_rule_subtest();
    env_utf8_subtest();
#if defined(__ANDROID__)
    android_refusals_subtest();
#else
    desktop_tool_subtest();
#endif
    mkdir_private_symlink_subtest();
    get_path_private_subtest();
    list_files_dot_subtest();
    child_signal_state_subtest();
#if !defined(__ANDROID__)
    exec_failure_subtest();
#endif

    printf("[POSIX_PROCESS_TEST] ALL POSIX PROCESS TESTS PASSED!\n");
    return 0;
}

#endif /* !_WIN32 */
