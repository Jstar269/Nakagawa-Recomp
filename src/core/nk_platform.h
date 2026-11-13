/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_PLATFORM_H
#define NK_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standardized structured data directory categories */
typedef enum {
    NK_PATH_CONFIG,  /* User preferences & settings */
    NK_PATH_DATA,    /* Game library, manifests, installed assets */
    NK_PATH_CACHE,   /* Recompilation cache, shaders */
    NK_PATH_LOGS,    /* Runtime and application logs */
    NK_PATH_SAVES    /* PSP Memory Stick save data (ms0/PSP/SAVEDATA) */
} NkPathType;

/* 64-bit file seek and tell portable across Win32, Linux, and macOS */
int nk_fseek64(FILE *f, int64_t offset, int whence);
int64_t nk_ftell64(FILE *f);

/* UTF-8 file primitives. A per-user profile directory holding non-ASCII
 * characters (an accented given name, a Japanese user name) is a legal path
 * that the narrow CRT reinterprets in the active ANSI code page, so the
 * file opens, removals and renames the player and the shared core make on
 * user-data and install paths go through these, or through an equivalent
 * wide-character open local to a translation unit that is built without this
 * platform layer. The shared nk_fopen_utf8 implementation lives in
 * nk_shared_helpers.c, which the player, runtime and tools link. Code that
 * opens such a path must not use the narrow fopen/remove/rename on Win32.
 * Win32 converts strictly (invalid UTF-8 fails rather than opening a path
 * where the bad bytes became U+FFFD); POSIX file names are already UTF-8 bytes,
 * so these are the plain system calls.
 *
 * nk_remove_utf8 removes a FILE only: it fails on a directory on every host
 * (DeleteFileW cannot remove one; POSIX uses unlink rather than remove). One
 * difference remains by design of the host: a read-only file is removed on
 * POSIX but fails on Win32 until the caller clears the attribute, so callers
 * must not depend on removing read-only files. */
FILE *nk_fopen_utf8(const char *path, const char *mode);
int nk_remove_utf8(const char *path);
int nk_rename_utf8(const char *from, const char *to);

/* Filesystem and path utilities (all paths in UTF-8) */
char nk_platform_path_separator(void);
/* The separator between entries of a PATH-style list: ';' on Windows, ':' elsewhere. */
char nk_platform_path_list_separator(void);
bool nk_platform_file_exists(const char *path);
bool nk_platform_dir_exists(const char *path);
/* True only when the OS says the name does not exist: ENOENT or ENOTDIR on POSIX, and
 * ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND on Win32. Any other failure (access denied, an
 * unreadable parent) is not missing, so a caller about to delete a name must not take it for
 * gone. An entry of any kind, a directory included, is not missing. */
bool nk_platform_path_missing(const char *path);
/* Call fn(name, ctx) for each regular file directly inside dir (names in UTF-8,
 * unsorted; subdirectories are skipped). Stops early when fn returns false.
 * Returns false when dir cannot be opened. */
typedef bool (*NkDirFileFn)(const char *name, void *ctx);
bool nk_platform_list_files(const char *dir, NkDirFileFn fn, void *ctx);
/* Call fn(name, is_dir, ctx) for each regular file and each directory directly
 * inside dir (names in UTF-8, unsorted, hidden names included; "." and ".." are
 * not entries). A symlink, junction or other reparse point is neither and is
 * skipped, so a walk built on this never leaves the tree it starts in. Stops
 * early when fn returns false. Returns false when dir cannot be opened. */
typedef bool (*NkDirEntryFn)(const char *name, bool is_dir, void *ctx);
bool nk_platform_list_dir(const char *dir, NkDirEntryFn fn, void *ctx);
/* nk_platform_list_dir for a caller that must account for every entry, such as
 * a backup. *skipped (required) counts what nk_platform_list_dir leaves out
 * without saying so: a symlink, junction or other reparse point (a cloud
 * placeholder file among them), a file a cloud service keeps off this
 * computer, any other entry that is neither a regular file nor a directory, an
 * entry whose details cannot be read, and a name or path too long for this
 * build. Unlike nk_platform_list_dir it also returns false when the listing
 * stops on a read error part way. */
bool nk_platform_list_dir_counted(const char *dir, NkDirEntryFn fn, void *ctx,
                                  size_t *skipped);
/* Flush f's buffered bytes and have the operating system put them on the disk
 * (fflush, then FlushFileBuffers or fsync). False when either fails. */
bool nk_platform_sync_file(FILE *f);
/* Put dir's own entries (names added, renamed or removed in it) on the disk:
 * fsync on the directory on POSIX, where a file system that cannot sync a
 * directory (EINVAL) counts as done. NTFS journals directory changes and
 * nk_rename_utf8 renames with MOVEFILE_WRITE_THROUGH, so Win32 has nothing to
 * do and returns true when dir exists. */
bool nk_platform_sync_dir(const char *dir);
/* Call fn(root, ctx) for the root of each drive the computer has mounted that a user could
 * plug in or carry: on Win32 the root of every fixed or removable drive letter; on macOS each
 * folder in /Volumes; elsewhere each folder in /media, /media/<name>, /run/media/<name> and
 * /mnt. Network and optical drives are left out. Stops early when fn returns false. Nothing is
 * opened or read here; on Win32, a drive with no medium does not raise a system dialog while fn
 * runs. */
void nk_platform_list_drive_roots(NkDirFileFn fn, void *ctx);
/* The bytes of every regular file under path (path itself when it is a file),
 * without following a symlink, junction or other reparse point: one counts as
 * nothing. A top-level child of path named skip_child (NULL for none) is left
 * out. A missing path is 0 bytes and true; false means a part could not be read. */
bool nk_platform_tree_size(const char *path, const char *skip_child, uint64_t *out_bytes);
/* Delete path and everything under it, without following a symlink, junction or
 * other reparse point: the link itself is removed, never its target. A
 * top-level child named skip_child (NULL for none) is kept, and then so is
 * path. A missing path is already gone: true. */
bool nk_platform_remove_tree(const char *path, const char *skip_child);
int64_t nk_platform_get_file_size(const char *path);
bool nk_platform_mkdir_p(const char *dir_path);
/* Create a directory tree and private files for user-owned source material.
 * POSIX uses owner-only permissions and refuses a final symlink; Win32 uses
 * the user's application-data ACLs through the wide-character backend. */
bool nk_platform_mkdir_p_private(const char *dir_path);
FILE *nk_platform_fopen_private(const char *path, const char *mode);

/* An exclusive lock on a file that other programs lock too: Win32 locks byte 0
   with LockFileEx, which excludes Python's msvcrt.locking on it; POSIX uses
   flock, which excludes Python's fcntl.flock. The file is created when missing
   and left in place. The OS releases the lock when its holder exits. */
typedef struct {
    void *native_handle;  /* Win32 */
    int fd;               /* POSIX */
    bool held;
} NkFileLock;

/* One running program per name (#2006): the player takes one per data folder, so a second
   start on the same library and settings finds the first instead of opening beside it. */
typedef enum {
    NK_INSTANCE_ACQUIRED = 0,  /* this process holds it now */
    NK_INSTANCE_RUNNING,       /* another live process holds it; see owner_pid */
    NK_INSTANCE_UNAVAILABLE    /* the OS refused the objects: run unguarded, say so */
} NkInstanceResult;

typedef struct {
    void *mutex;    /* Win32: the named mutex */
    void *mapping;  /* Win32: a named page holding the holder's process ID */
    bool held;
    int owner_pid;  /* after NK_INSTANCE_RUNNING: the holder's process ID, 0 when not yet known */
} NkSingleInstance;

/* Win32: a named mutex in this sign-in session, named by a hash of `name` (case, slash
   direction and trailing separators ignored, as Windows paths compare). A holder that ended
   without releasing it (a crash) leaves it abandoned, and the next start takes it over. A
   mutex belongs to a thread: acquire and release on the same thread, and a second acquire on
   the holding thread succeeds again.
   POSIX: there is no guard yet. Every call returns NK_INSTANCE_ACQUIRED and a second start
   opens beside the first. */
NkInstanceResult nk_platform_single_instance_acquire(NkSingleInstance *instance, const char *name);
/* Release and close what acquire opened; safe on a zeroed or already released instance. */
void nk_platform_single_instance_release(NkSingleInstance *instance);

/* Take the lock on path, trying again every 50 ms for up to timeout_ms (0 tries
   once). False when another holder keeps it that long or the file cannot be
   opened. */
bool nk_platform_file_lock_acquire(NkFileLock *lock, const char *path, unsigned timeout_ms);
/* Release a lock; one not held is left alone. */
void nk_platform_file_lock_release(NkFileLock *lock);

/* Structured path routing */
bool nk_platform_get_path(NkPathType type, char *out_path, size_t max_len);

/* Point every per-user folder of this process at `path` instead of the
 * platform default, the player's counterpart of nk_cli's --user-data-root:
 * NK_PATH_DATA (the library, title manifests, packages, staged games, fonts)
 * is `path` itself, and NK_PATH_CONFIG, NK_PATH_CACHE, NK_PATH_LOGS and
 * NK_PATH_SAVES are its config, cache, logs and saves folders, so nothing of
 * the default profile is read or written. NULL or "" restores the default.
 * Returns false, leaving the previous setting, when the path is too long.
 * Call before the first lookup. */
bool nk_platform_set_app_data_dir_override(const char *path);
/* The host's local time zone: its standard offset from UTC in minutes (east of UTC positive)
 * and whether daylight saving is in effect now. False when the host does not say. */
bool nk_platform_local_time_zone(int *standard_minutes, bool *daylight_now);
/* The operating system's own display preferences, read on every call so a change takes effect
 * without a restart. On Windows, reduced motion is "Animation effects" turned off
 * (SPI_GETCLIENTAREAANIMATION) and high contrast is a contrast theme in use (SPI_GETHIGHCONTRAST).
 * Elsewhere the player reads no such setting and both are false. */
bool nk_platform_prefers_reduced_motion(void);
bool nk_platform_high_contrast(void);
/* Resolve the canonical per-user data directory without creating it. */
bool nk_platform_resolve_app_data_dir(char *out_path, size_t max_len);
/* nk_platform_get_path's folder for `type`, without creating it. */
bool nk_platform_resolve_path(NkPathType type, char *out_path, size_t max_len);
/* This program's own executable, as UTF-8 (GetModuleFileNameW on Windows,
 * /proc/self/exe on Linux). False where the platform does not say. */
bool nk_platform_get_executable_path(char *out_path, size_t max_len);
/* Get/create the canonical per-user data directory. */
bool nk_platform_get_app_data_dir(char *out_path, size_t max_len);
/* Resolve the pre-canonical Windows profile location used by older builds.
 * Returns false on non-Windows hosts or when USERPROFILE is unavailable. */
bool nk_platform_get_legacy_app_data_dir(char *out_path, size_t max_len);
/* The default per-user folders of a Linux or macOS install (#324, nk_data_migrate.h): both
 * layouts' folders, whether each exists, and the one this process uses, decided by the rule there
 * the first time a folder is looked up. False, with *out cleared, where there is one layout (Windows,
 * Android) or an override names the folders (--user-data-root, portable mode). */
struct NkUserDataLayout;
bool nk_platform_user_data_layout(struct NkUserDataLayout *out);

/* The user's Desktop folder, where a first run's file pickers open. Windows:
 * FOLDERID_Desktop, which follows a Desktop the user or a sync tool moved.
 * POSIX: the Desktop folder in the home folder when it exists, else the home
 * folder itself. False when there is none. */
bool nk_platform_get_desktop_dir(char *out_path, size_t max_len);
/* The user's personal folders. */
typedef enum {
    NK_PERSONAL_DOCUMENTS = 0,
    NK_PERSONAL_DOWNLOADS,
    NK_PERSONAL_MUSIC,
    NK_PERSONAL_PICTURES,
    NK_PERSONAL_VIDEOS,
    NK_PERSONAL_DESKTOP,
    NK_PERSONAL_COUNT
} NkPersonalDir;
/* Where one of them is. Windows: its known folder (FOLDERID_Documents and so on),
 * which follows a folder the user or a sync tool moved. POSIX: the folder the
 * desktop's user-dirs.dirs names (XDG_DOCUMENTS_DIR and so on, in
 * $XDG_CONFIG_HOME or ~/.config), so a localized name is found, else the folder of
 * the English name in the home folder. False when it does not exist. Unlike
 * nk_platform_get_desktop_dir, NK_PERSONAL_DESKTOP never falls back to the home
 * folder. */
bool nk_platform_get_personal_dir(NkPersonalDir which, char *out_path, size_t max_len);
#if defined(_WIN32) || defined(_WIN64)
/* Unit-testable Windows base selection: LOCALAPPDATA, Known Folder, APPDATA.
 * USERPROFILE is deliberately not a candidate. */
bool nk_platform_resolve_windows_data_base(
    const char *known_local_app_data,
    const char *local_app_data,
    const char *roaming_app_data,
    char *out_base,
    size_t max_len
);
#endif

/* Resolve `path` to an absolute path in `out_path`.
 *
 * The runtime refuses a relative SR_DATAROOT ("configured but is not a valid
 * absolute path") and then declines to build an index at all, so a launch
 * assembled from a relative working directory silently lost its data root.
 * Returns false and leaves `out_path` untouched when the path cannot be
 * resolved. Win32 uses GetFullPathNameA (which does not require the path to
 * exist); POSIX uses realpath (which does), so callers should resolve paths
 * they have already confirmed. */
bool nk_platform_absolute_path(const char *path, char *out_path, size_t max_len);

/* The path the existing file or folder at `path` really has, every symlink,
 * junction, mount alias (subst), short 8.3 name and "." or ".." resolved, so two
 * names of one folder compare equal. POSIX: realpath. Win32: the handle's final
 * path (GetFinalPathNameByHandleW), without its \\?\ prefix. False when the path
 * does not exist or cannot be resolved. */
bool nk_platform_real_path(const char *path, char *out_path, size_t max_len);

/* Open the regular file at `path` for reading in binary mode, refusing anything
 * else: a symlink, junction or other reparse point (a cloud placeholder among
 * them) is not followed, and a folder, a device or a FIFO is not opened. Checked on
 * the open handle, so the file cannot be swapped for a link between a check and the
 * open. NULL on refusal or failure. */
FILE *nk_platform_open_regular_file(const char *path);

/* Whether `path` is longer than the file APIs this process uses accept, so a
 * create or open of it fails for its length alone. On Win32 that is MAX_PATH
 * (260 with the terminator; 248 for a new directory) unless the process has
 * long paths enabled; on POSIX, PATH_MAX. A caller asks only after the create
 * or open failed, to tell a too-long path apart from a damaged input. */
bool nk_platform_path_exceeds_limit(const char *path, bool is_directory);

/* Process handle abstraction */
typedef struct {
    void *native_handle;
    void *job_handle;
    int process_id;
    bool is_active;
    /* A POSIX child can only be reaped once. nk_platform_is_process_running
       reaps it to learn that it exited, which would otherwise throw the exit
       status away and leave nk_platform_wait_process nothing to report. The
       status is cached here instead. Win32 keeps the handle open and reads the
       code on demand, so it never sets these. */
    bool has_cached_exit;
    int cached_exit_code;
    /* Why the last nk_platform_spawn_process into this handle failed, as the
       OS said it: GetLastError() after CreateProcessW on Win32, errno on POSIX.
       0 when the spawn succeeded or failed before the OS was asked. */
    int spawn_error;
    /* Set by nk_platform_spawn_tool on Android, where a tool program is an ordinary forked
       child and a title is another app process: the process calls below take this handle
       down the fork/exec route instead of the title route. False everywhere else. */
    bool tool_child;
} NkProcessHandle;

/* What the OS error a refused process start carries says about the refusal.
   Only Win32 reports these; every POSIX error reads as NK_SPAWN_BLOCK_NONE. */
typedef enum {
    NK_SPAWN_BLOCK_NONE = 0,
    /* ERROR_ACCESS_DENIED. Microsoft Defender's attack surface reduction rules
       refuse a program this way, as do file permissions. */
    NK_SPAWN_BLOCK_ACCESS_DENIED,
    /* ERROR_VIRUS_INFECTED or ERROR_VIRUS_DELETED: the antivirus found a threat. */
    NK_SPAWN_BLOCK_THREAT,
    /* ERROR_ACCESS_DISABLED_BY_POLICY, ERROR_ACCESS_DISABLED_NO_SAFER_UI_BY_POLICY or
       ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION: a software restriction, AppLocker or
       App Control policy. */
    NK_SPAWN_BLOCK_POLICY
} NkSpawnBlock;

NkSpawnBlock nk_platform_spawn_block(int os_error);

/* The OS's own one-line text for an error code (FormatMessageW on Win32,
   strerror on POSIX), without a trailing newline. False when it has none. */
bool nk_platform_describe_os_error(int os_error, char *out, size_t out_size);

/* Spawn a child process with specified arguments, environment variables, and working directory.
 * argv: NULL-terminated array of arguments (argv[0] is program path)
 * envp: NULL-terminated array of "KEY=VAL" strings, or NULL to inherit current environment
 * working_directory: working directory path, or NULL to inherit current
 * output_log_path: optional append-only destination for stdout and stderr; when
 *                  absent, Windows sends those streams to NUL and creates no console
 */
bool nk_platform_spawn_process(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    const char *output_log_path,
    NkProcessHandle *out_process
);

/* The scheduling priority a spawned process starts at. Its children inherit it, so a
 * low-priority package build keeps every compiler it starts low too. */
typedef enum {
    NK_PROCESS_PRIORITY_NORMAL = 0,
    /* Win32 BELOW_NORMAL_PRIORITY_CLASS; POSIX nice NK_PLATFORM_LOW_PRIORITY_NICE. A game
       at normal priority then always runs first, and the process uses what is left. */
    NK_PROCESS_PRIORITY_LOW
} NkProcessPriority;

/* The POSIX nice value of NK_PROCESS_PRIORITY_LOW. */
#define NK_PLATFORM_LOW_PRIORITY_NICE 10

/* nk_platform_spawn_process, with the child started at `priority`. The child never runs
 * a single instruction at a higher priority: Win32 sets the class at creation and POSIX
 * before exec. A priority the OS refuses fails the spawn. */
bool nk_platform_spawn_process_at_priority(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    const char *output_log_path,
    NkProcessPriority priority,
    NkProcessHandle *out_process
);

/* The priority a running child has now, as NK_PROCESS_PRIORITY_LOW when it runs below
 * normal (any lower class or nice value counts) and NORMAL otherwise. False when the OS
 * cannot say, or for an Android title, which runs in another app process rather than as a
 * child (an Android tool child reads as on the desktop). */
bool nk_platform_process_priority(const NkProcessHandle *process, NkProcessPriority *out);

/* Starts one of the application's own tool programs (the package build's Python, compiler and
 * make, an archive tool), with the same arguments and handle as nk_platform_spawn_process.
 * On the desktop it is nk_platform_spawn_process. On Android an app may run only programs its
 * APK ships, from the native library folder (it may not execute files it wrote: #360), and a
 * title is started through TitleActivity instead; there it forks and execs executable_path only
 * when nk_platform_path_is_tool_in_dir() accepts it for nk_platform_get_tool_dir(), and fails
 * with spawn_error EACCES for any other program. */
bool nk_platform_spawn_tool(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    const char *output_log_path,
    NkProcessHandle *out_process
);

/* nk_platform_spawn_tool, with the child started at `priority` as
 * nk_platform_spawn_process_at_priority starts it. On Android the tool-folder rule applies
 * first, at every priority, and a low tool child is niced before exec as on the desktop. */
bool nk_platform_spawn_tool_at_priority(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    const char *output_log_path,
    NkProcessPriority priority,
    NkProcessHandle *out_process
);

/* The folder the application's bundled tool programs are in: on Android the folder this code
 * was loaded from, which is the APK's native library folder. False on hosts that bundle no
 * tools there (the desktop finds its tools on PATH or in the prerequisites folder). */
bool nk_platform_get_tool_dir(char *out_path, size_t max_len);

/* True when path resolves, symbolic links followed, to a regular file directly inside dir (not
 * in a folder below it): the rule nk_platform_spawn_tool applies on Android. Windows has no
 * bundled tool folder, and its backend answers false. */
bool nk_platform_path_is_tool_in_dir(const char *dir, const char *path);

bool nk_platform_is_process_running(NkProcessHandle *process);
int nk_platform_wait_process(NkProcessHandle *process, int timeout_ms);
/* Gives the foreground to the child's visible top-level window. The caller must
   be the foreground process, as a launcher that just started the child is.
   False when the child has no such window yet, or where there is no window
   system to ask (the POSIX build). */
bool nk_platform_bring_process_window_to_front(const NkProcessHandle *process);
/* Ends the process and its children, then waits a bounded time for it to go, so a stopped
   process is not still writing once the caller goes on. POSIX sends SIGTERM, then SIGKILL,
   and reaps the child (up to about 3 s). Win32 terminates its job and the process and waits
   up to NK_PLATFORM_TERMINATE_WAIT_MS; is_active stays true there when it had not ended by
   then. The caller's thread blocks for that wait. */
#define NK_PLATFORM_TERMINATE_WAIT_MS 2000
void nk_platform_terminate_process(NkProcessHandle *process);
void nk_platform_close_process(NkProcessHandle *process);
/* Whether nk_platform_bring_process_window_to_front can work on this host at all. Win32 can;
   no POSIX backend raises another process's window (X11 and Wayland leave focus to the
   window manager), so a false from the raise there is expected rather than a failure. */
bool nk_platform_can_front_process_windows(void);

/* ---- Host seam ------------------------------------------------------------------------
 *
 * Code outside the nk_platform_*.c backends asks these instead of testing _WIN32,
 * __APPLE__, __linux__ or __ANDROID__ itself (docs/PLATFORM_PORTABILITY.md, "One platform
 * seam"; tools/test_platform_seam.py keeps the count of such tests from growing). */

/* The host this build runs on. User-facing text that names a host's own tools or folders
   chooses its wording from this at run time, so every variant compiles, and is tested, on
   every host. */
typedef enum {
    NK_HOST_WINDOWS = 0,
    NK_HOST_LINUX,
    NK_HOST_MACOS,
    NK_HOST_ANDROID,
    NK_HOST_OTHER_POSIX
} NkHostKind;

NkHostKind nk_platform_host_kind(void);

/* Path text comparison as the host's file system compares names: Win32 folds ASCII case
   and treats '/' and '\\' as one separator; POSIX compares bytes. Used to find a folder's
   text inside other text (the profile folder in a message, a root above a path). */
bool nk_platform_path_chars_equal(char a, char b);
/* '\\' or '/' on Win32; '/' on POSIX. */
bool nk_platform_is_path_separator(char c);
/* Whether `path` is absolute as written. Win32: a drive letter, ':' and a separator, or two
   separators (a UNC or \\?\ path); POSIX: a leading '/'. The text alone: nothing is resolved
   or looked up. False for NULL. */
bool nk_platform_path_is_absolute(const char *path);

/* The raw environment values naming the user: the profile folder (USERPROFILE on Win32,
   HOME on POSIX) and the account name (USERNAME on Win32, USER on POSIX). NULL when unset.
   Not resolved, created or checked: callers that need the per-user folders use
   nk_platform_get_path. */
const char *nk_platform_env_home_dir(void);
const char *nk_platform_env_user_name(void);

/* This process's id (_getpid on Win32, getpid on POSIX). */
unsigned long nk_platform_process_id(void);

/* The client end of a named pipe another program on this PC serves: local IPC, never a
   network connection. Discord Rich Presence (#2259) talks to the Discord app through one.
   Win32: the named pipe <name> on this PC (the local pipe namespace only), opened for
   overlapped I/O so a write can give up and no read waits, at identification level only, so
   the program serving the pipe cannot act as this user. POSIX: unsupported; supported() is
   false and open fails. */
typedef struct {
    void *handle;  /* Win32: the pipe's HANDLE */
    bool open;
} NkLocalPipe;

bool nk_platform_local_pipe_supported(void);
/* Connect to the pipe `name` (no directory: "discord-ipc-0"). False, quietly, when nobody
   serves it or it is busy. */
bool nk_platform_local_pipe_open(NkLocalPipe *pipe, const char *name);
/* Write all `len` bytes within timeout_ms. False on a short write, a broken pipe or the
   timeout; the caller then closes the pipe. */
bool nk_platform_local_pipe_write(NkLocalPipe *pipe, const void *data, size_t len,
                                  unsigned timeout_ms);
/* Read what has already arrived, up to cap bytes, without waiting: the count read, 0 when
   nothing is waiting, -1 when the other end closed the pipe or it failed. */
long nk_platform_local_pipe_read(NkLocalPipe *pipe, void *buf, size_t cap);
/* Close it; safe on a zeroed or closed pipe. */
void nk_platform_local_pipe_close(NkLocalPipe *pipe);

/* The OS error code of the last failed nk_platform or nk_*_utf8 file call on this thread:
   GetLastError() on Win32, errno on POSIX. Read it straight after the failing call. */
int nk_platform_last_os_error(void);

/* The first system UI font file found (Win32: the open-source fonts in the per-user fonts
   folder, then %WINDIR%\Fonts; macOS and Linux: a short table of system font paths).
   False when none exists. */
bool nk_platform_find_system_ui_font(char *out_path, size_t max_len);

/* Make main's arguments UTF-8. Win32: the console's output code page becomes UTF-8 and argv
   is rebuilt from the UTF-16 command line (the narrow argv is in the ANSI code page); the
   new array lives until the process exits. POSIX arguments already are UTF-8 bytes: nothing
   changes. False, leaving both untouched, when the command line cannot be converted. */
bool nk_platform_init_utf8_main(int *argc, char ***argv);

/* Give a GUI program usable stdout and stderr. Win32 (a -mwindows program has no console):
   standard handles it was started with are kept; otherwise the streams go to the parent
   terminal's console when there is one, else to `fallback_log_name` in the NK_PATH_LOGS
   folder, rewritten each run. Both streams are then unbuffered, since the Windows CRT has no
   line buffering (#1303). POSIX streams already reach the terminal or the launcher's log:
   nothing changes. Call after nk_platform_set_app_data_dir_override. */
void nk_platform_setup_standard_streams(const char *fallback_log_name);

/* Process start, for hosts whose application model needs it. Android: copy stdout and stderr to
   the system log under log_tag (and to NK_ANDROID_LOG_FILE when set), and make the folder the app
   unpacked its bundled files to (NK_ANDROID_APP_DIR) the working directory, which is where
   SDL_GetBasePath() points there. Everywhere else it does nothing and returns true. False when
   the Android folder is missing; the process can still run but finds no bundled files. */
bool nk_platform_host_startup(const char *log_tag);

/* Process end, for a process that ends without exit()'s cleanup (Android's title process, see
   android/app/src/main/cpp/nk_android_title.c). Android: flush stdout and stderr, and wait until
   nk_platform_host_startup's copy has written every line to the system log and the log file, so
   the last lines of a run are not lost; the streams go nowhere afterwards. Everywhere else it
   does nothing. */
void nk_platform_host_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NK_PLATFORM_H */
