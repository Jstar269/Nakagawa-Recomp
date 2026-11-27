// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import android.content.Context;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * One title's runtime, in the ":title" process. SDL calls nk_android_title_main() in the title's
 * library (android/app/src/main/cpp/nk_android_title.c), which runs the runtime's main() with the
 * launcher's arguments and ends the process when the run ends, as a desktop child process ends.
 *
 * <p>Extras: library (a bare name such as nk_title_showcase-scene-v1 or a path in the native
 * library folder for a title the APK ships; the absolute path of a title the player built, in the
 * app's files folder), args (String[]), env (String[] of KEY=VALUE), cwd, log. TitleLibrary decides
 * which libraries may load; any other refuses the launch with exit status {@link #EXIT_REFUSED}.
 * A library that does not start records {@link #EXIT_NOT_STARTED}, so the launcher never waits
 * for a run that did not begin. NK_ANDROID_EXIT_FILE and NK_ANDROID_PID_FILE in env are taken
 * only when they name the launcher's status files (TitleLibrary.launchFile).
 */
public class TitleActivity extends SDLActivity {
    static final String EXTRA_LIBRARY = "library";
    static final String EXTRA_ARGS = "args";
    static final String EXTRA_ENV = "env";
    static final String EXTRA_CWD = "cwd";
    static final String EXTRA_LOG = "log";
    /** The exit status of a refused library: what a POSIX shell gives a command it cannot run. */
    static final int EXIT_REFUSED = 126;
    /**
     * The exit status of a library that did not start (SDL could not load it, or it has no
     * nk_android_title_main): what a POSIX shell gives a command it cannot find.
     */
    static final int EXIT_NOT_STARTED = 127;

    /** The launcher's exit file for this launch, from the env extra, or null. */
    private String mExitFile;
    private String mLibraryName = "";
    private String mLibraryPath = "";
    private String[] mArgs = new String[0];

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        NakagawaHost.prepare(this);
        String library = getIntent().getStringExtra(EXTRA_LIBRARY);
        String[] args = getIntent().getStringArrayExtra(EXTRA_ARGS);
        String[] env = getIntent().getStringArrayExtra(EXTRA_ENV);
        String cwd = getIntent().getStringExtra(EXTRA_CWD);
        String log = getIntent().getStringExtra(EXTRA_LOG);
        if (args != null) mArgs = args;
        if (env != null) {
            for (String entry : env) {
                int eq = entry.indexOf('=');
                if (eq <= 0) continue;
                String name = entry.substring(0, eq);
                String value = entry.substring(eq + 1);
                // The status files are written by renaming over them; a debug build lets any
                // app start this activity, so only the launcher's own names are taken.
                String suffix = name.equals("NK_ANDROID_EXIT_FILE") ? "exit"
                        : name.equals("NK_ANDROID_PID_FILE") ? "pid" : null;
                if (suffix != null && TitleLibrary.launchFile(value, suffix) == null) {
                    Log.e(NakagawaHost.TAG, "TITLE_ACTIVITY ignored " + name + "=" + value
                            + ": not one of the launcher's title-launch status files");
                    continue;
                }
                if ("exit".equals(suffix)) mExitFile = value;
                NakagawaHost.setenv(name, value);
            }
        }
        if (library != null) {
            TitleLibrary.Choice choice = TitleLibrary.resolve(
                    library, getApplicationInfo().nativeLibraryDir, getFilesDir().getPath());
            if (choice == null) {
                Log.e(NakagawaHost.TAG, "TITLE_ACTIVITY refused library " + library
                        + ": not a library the APK ships nor one built in the app's files folder");
                endProcess(EXIT_REFUSED);   // does not return
                return;
            }
            mLibraryName = choice.bundledName;
            mLibraryPath = choice.path;
        }
        // The working directory stays the app's files folder, where SDL also resolves relative
        // paths (NakagawaHost). A desktop child starts in its package folder; the runtime
        // reads nothing relative to it that the arguments and environment do not name.
        if (cwd != null) Log.i(NakagawaHost.TAG, "TITLE_ACTIVITY package folder " + cwd);
        if (log != null) NakagawaHost.setenv("NK_ANDROID_LOG_FILE", log);
        Log.i(NakagawaHost.TAG, "TITLE_ACTIVITY library=" + mLibraryPath + " args=" + mArgs.length);
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            // SDL could not load a library (its message is in the log above it) and shows its own
            // dialog, which would leave this process alive with no status for the launcher.
            Log.e(NakagawaHost.TAG, "TITLE_LIBRARY_LOAD_FAILED library=" + mLibraryPath
                    + ": the runtime did not start");
            endProcess(EXIT_NOT_STARTED);
        }
    }

    /**
     * On SDL's thread. nk_android_title_main ends the process itself when the run ends
     * (nk_android_title.c), so SDL's main returns here only when it never ran: SDL could not
     * dlopen() the library, or the library has no nk_android_title_main.
     */
    @Override
    protected void main() {
        super.main();
        Log.e(NakagawaHost.TAG, "TITLE_MAIN_NOT_RUN library=" + mLibraryPath
                + ": nk_android_title_main did not run, so the runtime did not start");
        endProcess(EXIT_NOT_STARTED);
    }

    /**
     * Ends this process with {@code status}, as a desktop child ends when its program cannot run:
     * the status goes to NK_ANDROID_EXIT_FILE, which the launcher reads
     * (nk_platform_is_process_running in src/core/nk_platform_posix.c).
     */
    private void endProcess(int status) {
        if (mExitFile != null && !TitleLibrary.writeStatus(mExitFile, status)) {
            Log.e(NakagawaHost.TAG, "TITLE_ACTIVITY could not record the exit status " + status
                    + " in " + mExitFile);
        }
        Process.killProcess(Process.myPid());
        System.exit(status);
    }

    /** SDL3 always; a library the APK ships by name, while SDL dlopen()s a built one by path. */
    /** SDL's surface, without its accelerometer request on a device that has none. */
    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new NakagawaHost.Surface(context);
    }

    @Override
    protected String[] getLibraries() {
        if (mLibraryName.isEmpty()) return new String[] { "SDL3" };
        return new String[] { "SDL3", mLibraryName };
    }

    @Override
    protected String getMainSharedObject() {
        return mLibraryPath;
    }

    @Override
    protected String getMainFunction() {
        return "nk_android_title_main";
    }

    @Override
    protected String[] getArguments() {
        return mArgs;
    }
}
