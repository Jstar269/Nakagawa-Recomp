/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Process start and end for a title's runtime library on Android (libnk_title_<id>.so).
 *
 * TitleActivity runs in its own app process (":title") and calls nk_android_title_main() with the
 * argv the launcher built, exactly what a desktop child process gets. This file adds what the
 * desktop gets from the operating system for free:
 *   - the pid, in NK_ANDROID_PID_FILE, so the launcher can follow a process that is not its child;
 *   - the exit status, in NK_ANDROID_EXIT_FILE, from a return out of main() or from exit(),
 *     _exit() or _Exit() anywhere in the runtime (the library is linked with --wrap for those);
 *   - the end of the process when the run ends, as on the desktop: the runtime's global state
 *     is never reused by a second run in the same process.
 *
 * The process is an app process, though, with Android's own threads running in it (hwui's
 * render threads among them). exit() would run every handler in the process, the framework's
 * C++ static destructors included, and a render thread then locks a destroyed mutex and aborts
 * the process (FORTIFY, SIGABRT). A desktop child has no such threads. So the runtime's atexit()
 * goes through the shim too: its handlers are kept here and run, last registered first, as exit()
 * would run them; the system log copy is drained (nk_platform_host_shutdown); and the process
 * ends with _exit(), which runs nobody else's. The exit status is recorded last, after the log
 * is drained, because the launcher takes the exit file as the end of the run.
 *
 * Guest threads call exit() too (sceKernelExitGame). The first thread to end the process owns
 * the exit; any other thread that calls exit(), _exit() or _Exit() meanwhile waits for the
 * process to end. A handler that calls exit() itself goes on with the handlers not yet run, then
 * ends the process with its own status, as each handler runs once. So a handler must not wait for
 * a thread that ends the process or registers a handler meanwhile: that thread waits on the exit
 * lock, and the two never finish (no handler the runtime registers does). The wrapped _exit()
 * and _Exit() take the lock too, so they are not for signal handlers.
 * See nk_platform_spawn_process in src/core/nk_platform_posix.c for the launcher side.
 */

#include "nk_platform.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv);   /* src/rt/driver.c */

void __real__exit(int status) __attribute__((noreturn));
void __real__Exit(int status) __attribute__((noreturn));

/* The runtime registers about a dozen; more than this is a programming error: atexit() fails
   and says so on stderr. */
#define NK_TITLE_EXIT_HANDLERS 64
static void (*s_exit_handlers[NK_TITLE_EXIT_HANDLERS])(void);
static int s_exit_handler_count;
/* Held from the start of an exit to the end of the process, never released. Recursive, so a
   handler that calls exit() on the exiting thread goes on; any other thread blocks here. It also
   guards the handler table, which atexit() fills while the runtime runs. */
static pthread_mutex_t s_exit_lock;
static pthread_once_t s_exit_lock_once = PTHREAD_ONCE_INIT;

static void init_exit_lock(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_exit_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void lock_exit(void) {
    pthread_once(&s_exit_lock_once, init_exit_lock);
    pthread_mutex_lock(&s_exit_lock);
}

static void write_number_file(const char *env_name, long value) {
    const char *path = getenv(env_name);
    char text[32];
    if (!path || !path[0]) return;
    int length = snprintf(text, sizeof(text), "%ld\n", value);
    if (length <= 0) return;
    /* Written to a temporary name and renamed, so the launcher never reads half a number. */
    char temp[1024];
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    ssize_t written = write(fd, text, (size_t)length);
    close(fd);
    if (written == (ssize_t)length) (void)rename(temp, path);
}

/* The last step before the process ends: the log is drained first, so the launcher, which takes
   the exit file as the end of the run, has every line by then. Called with s_exit_lock held. */
static void finish_process(int status) {
    fflush(NULL);
    nk_platform_host_shutdown();
    write_number_file("NK_ANDROID_EXIT_FILE", (long)(status & 0xff));
}

int __wrap_atexit(void (*handler)(void)) {
    if (!handler) return -1;
    lock_exit();
    int full = s_exit_handler_count == NK_TITLE_EXIT_HANDLERS;
    if (!full) s_exit_handlers[s_exit_handler_count++] = handler;
    pthread_mutex_unlock(&s_exit_lock);
    if (full) {
        fprintf(stderr, "nk_android_title: more than %d atexit handlers; this one will not run\n",
                NK_TITLE_EXIT_HANDLERS);
        return -1;
    }
    return 0;
}

__attribute__((noreturn)) void __wrap_exit(int status) {
    lock_exit();   /* never released: the process ends below */
    /* Each handler is taken off the table before it runs, so a handler that calls exit() goes on
       from the next one and never runs one twice. */
    while (s_exit_handler_count > 0) s_exit_handlers[--s_exit_handler_count]();
    finish_process(status);
    __real__exit(status);
}

__attribute__((noreturn)) void __wrap__exit(int status) {
    lock_exit();
    finish_process(status);
    __real__exit(status);
}

__attribute__((noreturn)) void __wrap__Exit(int status) {
    lock_exit();
    finish_process(status);
    __real__Exit(status);
}

__attribute__((visibility("default"))) int nk_android_title_main(int argc, char **argv) {
    write_number_file("NK_ANDROID_PID_FILE", (long)getpid());
    (void)nk_platform_host_startup("nakagawa-title");
    __wrap_exit(main(argc, argv));
}
