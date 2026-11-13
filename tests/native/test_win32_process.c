/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scratch root for the files this test writes: the checkout's build/ by default, or
 * the BUILD_ROOT the Makefile passes as -DSR_SELFTEST_BUILD_ROOT, so a scratch run never
 * touches the checkout. */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("[PROCESS_TEST] Starting Windows CreateProcessW argv quoting & env tests...\n");

#if defined(_WIN32) || defined(_WIN64)
    const char *helper_exe = SR_SELFTEST_BUILD_ROOT "\\argv_echo_helper.exe";

    /* Verify helper executable exists */
    assert(nk_platform_file_exists(helper_exe));

    /* The console-subsystem helper must report no console. This check fails
       before the process helper uses CREATE_NO_WINDOW on a desktop runner. */
    const char *console_probe_path = SR_SELFTEST_BUILD_ROOT "\\process_console_probe.txt";
    remove(console_probe_path);
    const char *console_probe_argv[] = {
        helper_exe, "--write-stdio-probe", console_probe_path, NULL
    };
    NkProcessHandle console_probe_process;
    assert(nk_platform_spawn_process(helper_exe, console_probe_argv, NULL, NULL,
                                     NULL, &console_probe_process));
    assert(nk_platform_wait_process(&console_probe_process, 5000) == 0);
    nk_platform_close_process(&console_probe_process);
    FILE *console_probe = fopen(console_probe_path, "rb");
    assert(console_probe != NULL);
    char console_probe_line[64];
    assert(fgets(console_probe_line, sizeof(console_probe_line), console_probe) != NULL);
    fclose(console_probe);
    remove(console_probe_path);
    assert(strcmp(console_probe_line, "CONSOLE_WINDOW=NULL\n") == 0);

    const char *stdio_log_path = SR_SELFTEST_BUILD_ROOT "\\process_stdio.log";
    remove(stdio_log_path);
    NkProcessHandle logged_process;
    assert(nk_platform_spawn_process(helper_exe, console_probe_argv, NULL, NULL,
                                     stdio_log_path, &logged_process));
    assert(nk_platform_wait_process(&logged_process, 5000) == 0);
    nk_platform_close_process(&logged_process);
    FILE *stdio_log = fopen(stdio_log_path, "rb");
    assert(stdio_log != NULL);
    char stdio_output[1024];
    size_t stdio_size = fread(stdio_output, 1, sizeof(stdio_output) - 1, stdio_log);
    fclose(stdio_log);
    stdio_output[stdio_size] = '\0';
    assert(strstr(stdio_output, "CONSOLE_WINDOW=NULL") != NULL);
    assert(strstr(stdio_output, "PROCESS_STDOUT_MARKER") != NULL);
    assert(strstr(stdio_output, "PROCESS_STDERR_MARKER") != NULL);
    remove(stdio_log_path);

    const char *out_file = SR_SELFTEST_BUILD_ROOT "\\argv_echo_out.txt";
    /* Sized for an absolute BUILD_ROOT: the scratch path is far longer than the default. */
    char out_arg[1024];
    int out_arg_len = snprintf(out_arg, sizeof(out_arg), "--output=%s", out_file);
    assert(out_arg_len > 0 && (size_t)out_arg_len < sizeof(out_arg));

    /* Test case array of distinct arguments */
    const char *test_argv[] = {
        helper_exe,
        out_arg,
        "simple",
        "with spaces",
        "trailing\\",
        "double_trailing\\\\",
        "\"quoted\"",
        "quote \"inside\" text",
        "", /* empty argument */
        "trailing backslash and spaces \\ ",
        "C:\\Games\\PSP Game\\",
        "日本語_テスト_パス\\",
        NULL
    };

    const char *test_env[] = {
        "TEST_ENV_KEY=Hello_Unicode_Environment_ワールド",
        NULL
    };

    /* We test spawning directly via nk_platform_spawn_process */
    NkProcessHandle proc;
    bool ok = nk_platform_spawn_process(
        helper_exe,
        test_argv,
        test_env,
        NULL,
        NULL,
        &proc
    );
    assert(ok);
    assert(proc.is_active);

    /* Wait for child process */
    int exit_code = nk_platform_wait_process(&proc, 5000);
    assert(exit_code == 0);
    nk_platform_close_process(&proc);

    printf("[PROCESS_TEST] Spawn and exit code 0 verified.\n");

    /* Read child output file directly */
    FILE *f = fopen(out_file, "r");
    assert(f != NULL);

    char line[1024];
    int checked_args = 0;
    bool env_verified = false;
    bool sysroot_verified = false;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            line[--len] = '\0';
        }

        if (strncmp(line, "ARG[", 4) == 0) {
            int idx = -1;
            char val[512] = {0};
            if (sscanf(line, "ARG[%d]=%511[^\n]", &idx, val) >= 1) {
                if (idx == 0) {
                    checked_args++;
                } else if (idx < 12) {
                    const char *expected = test_argv[idx];
                    if (strcmp(val, expected) != 0) {
                        fprintf(stderr, "[PROCESS_TEST] Mismatch at ARG[%d]: expected '%s', got '%s'\n", idx, expected, val);
                        assert(strcmp(val, expected) == 0);
                    }
                    checked_args++;
                }
            } else if (sscanf(line, "ARG[%d]=", &idx) == 1) {
                /* Empty argument case */
                if (idx == 8) {
                    assert(test_argv[idx][0] == '\0');
                    checked_args++;
                }
            }
        } else if (strncmp(line, "TEST_ENV_KEY=", 13) == 0) {
            assert(strstr(line, "Hello_Unicode_Environment") != NULL);
            env_verified = true;
        } else if (strcmp(line, "SYSTEMROOT_PRESENT=1") == 0) {
            sysroot_verified = true;
        }
    }
    fclose(f);
    assert(checked_args == 12);
    assert(env_verified);
    assert(sysroot_verified);

    printf("[PROCESS_TEST] All 12 argv roundtrip cases verified exactly!\n");
    printf("[PROCESS_TEST] Controlled Unicode environment & SystemRoot inheritance verified!\n");
    /* Malformed UTF-8 must fail the spawn, never be dropped: a dropped working
     * directory launches in the parent's directory, and a dropped variable
     * inherits the parent's value. */
    const char bad_utf8[] = {(char)0xC3, (char)0x28, '\0'};
    char bad_env_item[32];
    snprintf(bad_env_item, sizeof(bad_env_item), "TEST_ENV_KEY=%s", bad_utf8);
    const char *bad_env[] = {bad_env_item, NULL};
    const char *quiet_argv[] = {helper_exe, out_arg, NULL};
    NkProcessHandle refused;
    assert(!nk_platform_spawn_process(helper_exe, quiet_argv, NULL, bad_utf8, NULL, &refused));
    assert(!refused.is_active);
    assert(!nk_platform_spawn_process(helper_exe, quiet_argv, bad_env, NULL, NULL, &refused));
    assert(!refused.is_active);
    printf("[PROCESS_TEST] Invalid UTF-8 working directory and environment refused.\n");

    /* One variable far longer than a fixed conversion buffer must reach the child
     * intact: the package build prepends a toolchain folder to a PATH that can run
     * to the 32767-character variable limit. */
    {
        enum { LONG_VALUE_LEN = 20000 };
        const char *long_out_file = SR_SELFTEST_BUILD_ROOT "\\argv_echo_long_env.txt";
        char long_out_arg[1024];
        int long_out_arg_len = snprintf(long_out_arg, sizeof(long_out_arg), "--output=%s", long_out_file);
        assert(long_out_arg_len > 0 && (size_t)long_out_arg_len < sizeof(long_out_arg));
        char *long_env_item = (char *)malloc(sizeof("TEST_ENV_KEY=") + LONG_VALUE_LEN);
        char *long_expected = (char *)malloc(sizeof("\nTEST_ENV_KEY=") + LONG_VALUE_LEN + 2);
        assert(long_env_item && long_expected);
        char *long_value = long_env_item + sizeof("TEST_ENV_KEY=") - 1;
        for (int i = 0; i < LONG_VALUE_LEN; i++) long_value[i] = (char)('A' + (i % 26));
        long_value[LONG_VALUE_LEN] = '\0';
        memcpy(long_env_item, "TEST_ENV_KEY=", sizeof("TEST_ENV_KEY=") - 1);
        snprintf(long_expected, sizeof("\nTEST_ENV_KEY=") + LONG_VALUE_LEN + 2, "\nTEST_ENV_KEY=%s\n", long_value);

        const char *long_env[] = {long_env_item, NULL};
        const char *long_argv[] = {helper_exe, long_out_arg, NULL};
        remove(long_out_file);
        NkProcessHandle long_proc;
        assert(nk_platform_spawn_process(helper_exe, long_argv, long_env, NULL, NULL, &long_proc));
        assert(nk_platform_wait_process(&long_proc, 5000) == 0);
        nk_platform_close_process(&long_proc);

        FILE *long_f = fopen(long_out_file, "rb");
        assert(long_f != NULL);
        char *long_output = (char *)malloc(LONG_VALUE_LEN + 8192);
        assert(long_output != NULL);
        size_t long_size = fread(long_output, 1, LONG_VALUE_LEN + 8192 - 1, long_f);
        fclose(long_f);
        long_output[long_size] = '\0';
        assert(strstr(long_output, long_expected) != NULL);
        free(long_output);
        free(long_expected);
        free(long_env_item);
        remove(long_out_file);
    }
    printf("[PROCESS_TEST] A 20000-character environment entry reached the child intact.\n");

    /* A refused start keeps the code CreateProcessW gave, which is the only
     * thing a Windows Security block reaches the player as. A folder is refused
     * with ERROR_ACCESS_DENIED, the code an attack surface reduction rule gives;
     * a missing file with ERROR_FILE_NOT_FOUND, which is no block. */
    char build_dir[MAX_PATH];
    assert(GetFullPathNameA(SR_SELFTEST_BUILD_ROOT, MAX_PATH, build_dir, NULL) > 0);
    const char *dir_argv[] = {build_dir, NULL};
    assert(!nk_platform_spawn_process(build_dir, dir_argv, NULL, NULL, NULL, &refused));
    assert(!refused.is_active);
    assert(refused.spawn_error == ERROR_ACCESS_DENIED);
    assert(nk_platform_spawn_block(refused.spawn_error) == NK_SPAWN_BLOCK_ACCESS_DENIED);
    char os_text[256];
    assert(nk_platform_describe_os_error(refused.spawn_error, os_text, sizeof(os_text)));
    assert(os_text[0] != '\0' && os_text[strlen(os_text) - 1] != '\n');
    char missing_exe[MAX_PATH + 32];
    snprintf(missing_exe, sizeof(missing_exe), "%s\\no-such-program.exe", build_dir);
    const char *missing_argv[] = {missing_exe, NULL};
    assert(!nk_platform_spawn_process(missing_exe, missing_argv, NULL, NULL, NULL, &refused));
    assert(refused.spawn_error == ERROR_FILE_NOT_FOUND);
    assert(nk_platform_spawn_block(refused.spawn_error) == NK_SPAWN_BLOCK_NONE);
    assert(nk_platform_spawn_block(ERROR_VIRUS_INFECTED) == NK_SPAWN_BLOCK_THREAT);
    assert(nk_platform_spawn_block(ERROR_VIRUS_DELETED) == NK_SPAWN_BLOCK_THREAT);
    assert(nk_platform_spawn_block(ERROR_ACCESS_DISABLED_BY_POLICY) == NK_SPAWN_BLOCK_POLICY);
    printf("[PROCESS_TEST] A refused start keeps the OS error and names a security block.\n");

    /* Profile directories are read through the wide API: a LOCALAPPDATA outside
     * the active code page must round-trip as UTF-8, not as '?' bytes. An
     * explicit LOCALAPPDATA takes precedence over the Known Folder. */
    static WCHAR saved_lad[32768];
    DWORD saved_len = GetEnvironmentVariableW(L"LOCALAPPDATA", saved_lad, 32768);
    /* The probe root is the scratch root (SR_SELFTEST_BUILD_ROOT) made absolute, so a
     * scratch run keeps the profile tree out of the checkout's build/. */
    WCHAR wscratch_rel[1024];
    WCHAR wscratch[1024];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, SR_SELFTEST_BUILD_ROOT, -1,
                               wscratch_rel, 1024) > 0);
    DWORD scratch_len = GetFullPathNameW(wscratch_rel, 1024, wscratch, NULL);
    assert(scratch_len > 0 && scratch_len < 1024);
    WCHAR wbase[1200];
    /* Separators are passed as arguments so no literal reads as a UNC path. */
    const WCHAR *wsep = L"\\";
    _snwprintf(wbase, 1200, L"%ls%lsnk_env_\x65E5\x672C\x00E9", wscratch, wsep);
    wbase[1199] = L'\0';
    assert(_wputenv_s(L"LOCALAPPDATA", wbase) == 0);
    char expected[4096];
    assert(WideCharToMultiByte(CP_UTF8, 0, wbase, -1, expected, sizeof(expected), NULL, NULL) > 0);
    const char *sep = "\\";
    size_t exp_len = strlen(expected);
    snprintf(expected + exp_len, sizeof(expected) - exp_len, "%sNakagawa%scache", sep, sep);
    char got[4096];
    assert(nk_platform_get_path(NK_PATH_CACHE, got, sizeof(got)));
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "[PROCESS_TEST] profile path mismatch: expected '%s', got '%s'\n", expected, got);
        assert(strcmp(got, expected) == 0);
    }
    assert(nk_platform_dir_exists(got));
    assert(_wputenv_s(L"LOCALAPPDATA",
                      saved_len > 0 && saved_len < 32768 ? saved_lad : L"") == 0);
    printf("[PROCESS_TEST] Non-code-page profile directory resolved as UTF-8.\n");

    /* Process tree termination:
     * A child process spawns a grandchild process. When the child process is
     * terminated via nk_platform_terminate_process, the grandchild must also
     * be terminated (not left running). */
    printf("[PROCESS_TEST] Testing process tree termination...\n");
    const char *pid_file = SR_SELFTEST_BUILD_ROOT "\\test_grandchild.pid";
    remove(pid_file);

    const char *tree_argv[] = {
        helper_exe,
        "--spawn-grandchild",
        helper_exe,
        pid_file,
        NULL
    };

    NkProcessHandle tree_proc;
    bool tree_ok = nk_platform_spawn_process(
        helper_exe,
        tree_argv,
        NULL,
        NULL,
        NULL,
        &tree_proc
    );
    assert(tree_ok);
    assert(tree_proc.is_active);

    DWORD grandchild_pid = 0;
    for (int i = 0; i < 200; i++) {
        FILE *pf = fopen(pid_file, "r");
        if (pf) {
            if (fscanf(pf, "%lu", &grandchild_pid) == 1 && grandchild_pid > 0) {
                fclose(pf);
                break;
            }
            fclose(pf);
        }
        Sleep(20);
    }
    assert(grandchild_pid > 0);

    HANDLE hGrandchild = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, grandchild_pid);
    assert(hGrandchild != NULL);
    assert(WaitForSingleObject(hGrandchild, 0) == WAIT_TIMEOUT);

    nk_platform_terminate_process(&tree_proc);
    /* Terminate returns once the process has ended (nk_platform.h). */
    assert(!tree_proc.is_active);
    assert(WaitForSingleObject((HANDLE)tree_proc.native_handle, 0) == WAIT_OBJECT_0);
    nk_platform_close_process(&tree_proc);

    DWORD wait_res = WaitForSingleObject(hGrandchild, 2000);
    if (wait_res != WAIT_OBJECT_0) {
        TerminateProcess(hGrandchild, 1);
        CloseHandle(hGrandchild);
        remove(pid_file);
        fprintf(stderr, "[PROCESS_TEST] Grandchild PID %lu survived child termination!\n", grandchild_pid);
        assert(wait_res == WAIT_OBJECT_0);
    }
    CloseHandle(hGrandchild);
    remove(pid_file);
    printf("[PROCESS_TEST] Process tree termination verified: grandchild terminated!\n");

    /* Settings > Build speed: Background starts the build child below normal, and what
       the child starts inherits it (the build's Make and compilers). Before
       nk_platform_spawn_process_at_priority every child ran at the player's class. */
    {
        DWORD own_class = GetPriorityClass(GetCurrentProcess());
        bool runner_low = own_class == IDLE_PRIORITY_CLASS ||
                          own_class == BELOW_NORMAL_PRIORITY_CLASS;
        NkProcessPriority read_back = NK_PROCESS_PRIORITY_LOW;
        const char *sleep_argv[] = { helper_exe, "--sleep-ms", "30000", NULL };
        NkProcessHandle normal_proc;
        assert(nk_platform_spawn_process(helper_exe, sleep_argv, NULL, NULL, NULL,
                                         &normal_proc));
        assert(nk_platform_process_priority(&normal_proc, &read_back));
        assert(read_back == (runner_low ? NK_PROCESS_PRIORITY_LOW : NK_PROCESS_PRIORITY_NORMAL));
        nk_platform_terminate_process(&normal_proc);
        nk_platform_close_process(&normal_proc);

        remove(pid_file);
        NkProcessHandle low_proc;
        assert(nk_platform_spawn_process_at_priority(helper_exe, tree_argv, NULL, NULL, NULL,
                                                     NK_PROCESS_PRIORITY_LOW, &low_proc));
        assert(nk_platform_process_priority(&low_proc, &read_back));
        assert(read_back == NK_PROCESS_PRIORITY_LOW);
        DWORD low_grandchild_pid = 0;
        for (int i = 0; i < 200 && low_grandchild_pid == 0; i++) {
            FILE *pf = fopen(pid_file, "r");
            if (pf) {
                if (fscanf(pf, "%lu", &low_grandchild_pid) != 1) low_grandchild_pid = 0;
                fclose(pf);
            }
            if (low_grandchild_pid == 0) Sleep(25);
        }
        assert(low_grandchild_pid > 0);
        HANDLE low_grandchild = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                            FALSE, low_grandchild_pid);
        assert(low_grandchild != NULL);
        DWORD grandchild_class = GetPriorityClass(low_grandchild);
        assert(grandchild_class == BELOW_NORMAL_PRIORITY_CLASS ||
               (runner_low && grandchild_class == own_class));
        nk_platform_terminate_process(&low_proc);
        nk_platform_close_process(&low_proc);
        (void)WaitForSingleObject(low_grandchild, 2000);
        CloseHandle(low_grandchild);
        remove(pid_file);
        printf("[PROCESS_TEST] Low priority reaches the child and its children%s\n",
               runner_low ? " (the runner itself is below normal)" : "");
    }

    /* nk_platform_env_utf8 reads the wide environment, where the launcher puts SR_TEX_REPLACE: unset
     * is 0, set and empty is 1 (the launcher exports SR_TEX_REPLACE= for every run without a pack;
     * _putenv cannot make an empty variable, SetEnvironmentVariableW can), a folder name outside the
     * ANSI code page arrives in UTF-8, and a value that does not fit or is not valid UTF-16 is -1. */
    {
        const WCHAR *name = L"NK_ENV_UTF8_TEST";
        const char *expected = "pack_\xC3\xA9\xE6\xBC\xA2";    /* 10 bytes */
        char out[64];
        assert(SetEnvironmentVariableW(name, NULL) || GetLastError() == ERROR_ENVVAR_NOT_FOUND);
        out[0] = 'x';
        assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, sizeof(out)) == 0 && out[0] == '\0');
        assert(SetEnvironmentVariableW(name, L""));
        out[0] = 'x';
        assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, sizeof(out)) == 1 && out[0] == '\0');
        assert(SetEnvironmentVariableW(name, L"pack_\u00e9\u6f22"));
        assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, sizeof(out)) == 1 && strcmp(out, expected) == 0);
        assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, 11) == 1 && strcmp(out, expected) == 0);
        out[0] = 'x';
        assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, 10) == -1 && out[0] == '\0');
        const WCHAR lone_surrogate[] = { L'p', 0xD800, 0 };
        assert(SetEnvironmentVariableW(name, lone_surrogate));
        out[0] = 'x';
        assert(nk_platform_env_utf8("NK_ENV_UTF8_TEST", out, sizeof(out)) == -1 && out[0] == '\0');
        assert(SetEnvironmentVariableW(name, NULL));
        printf("[PROCESS_TEST] nk_platform_env_utf8 reads unset, empty, non-ASCII, too long and invalid values\n");
    }

    printf("[PROCESS_TEST] Process & quoting test PASSED successfully!\n");
#else
    printf("[PROCESS_TEST] Skipping Windows-specific tests on non-Windows host.\n");
#endif

    return 0;
}
