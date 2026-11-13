/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NATIVE_TEST_ISOLATION_H
#define NATIVE_TEST_ISOLATION_H

#include "nk_types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NATIVE_TEST_PATH_MAX 1024

/* Creates a per-run temporary root named by tag and process id, arranges for
 * removal at exit via atexit. Must be called before any fixture is written. */
void native_test_create_root(const char *tag);

/* Points per-user environment variables (LOCALAPPDATA/APPDATA on Windows,
 * XDG variables and HOME on POSIX) into the temporary root created by
 * native_test_create_root. */
void native_test_isolate_user_data_roots(void);

/* Guard: fails when the per-user cache root resolves outside the temporary
 * root. Call after native_test_isolate_user_data_roots. */
void native_test_assert_cache_root_isolated(void);

/* Guard: fails when the per-user config root resolves outside the temporary
 * root. Call after native_test_isolate_user_data_roots. */
void native_test_assert_config_root_isolated(void);

/* Captures the real config paths before isolation, for later verification that
 * the real config directories were not touched: on Linux and macOS the settings
 * folders of both data layouts (#324, nk_data_migrate.h), the one in use first. */
void native_test_capture_real_config_probe(void);

/* Captures again from the current environment, replacing the earlier capture.
 * Only the guard's own test uses it, after pointing HOME and the XDG variables
 * at a stand-in profile. */
void native_test_recapture_real_config_probe(void);

/* Whether every captured real config directory (and its input_profiles
 * subdirectory) is as it was at the capture; when not, problem names what changed. */
bool native_test_real_config_unchanged(char *problem, size_t problem_size);

/* Verifies the real config directories (and input_profiles subdirectories) were
 * not modified during the test run, and aborts naming the change when one was.
 *
 * Limitation note: when the real config directory or input_profiles directory
 * already existed before the test run, this probe checks that non-existent
 * test-created targets (such as the default disc profile UCUS98701.json) were
 * not created, and if pre-existing, compares their file size and modification
 * timestamp (mtime) against the pre-test snapshot. */
void native_test_assert_real_config_untouched(void);

/* Returns the path to the temporary root created for this run. */
const char *native_test_get_root(void);

/* Returns the real config probe path captured before isolation: the settings
 * folder of the layout in use, or "" when none could be named. */
const char *native_test_get_real_config_probe(void);

/* Returns the index-th captured real config path (0 is the layout in use, 1 the
 * other layout's on Linux and macOS), or "" past the last. */
const char *native_test_get_real_config_probe_at(size_t index);

/* Returns true when child lies within parent directory. */
bool native_test_path_within(const char *child, const char *parent);

/* Removes a directory tree without following links. Returns true on success,
 * or false on failure with an error diagnostic written to stderr. */
bool native_test_remove_tree(const char *path);

/* Helper to set or clear (value == NULL or empty "") an environment variable.
 * When clearing, removes the variable from both CRT (getenv) and Win32
 * (GetEnvironmentVariableW) environments on Windows, or via unsetenv on POSIX. */
void native_test_set_env(const char *name, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_TEST_ISOLATION_H */
