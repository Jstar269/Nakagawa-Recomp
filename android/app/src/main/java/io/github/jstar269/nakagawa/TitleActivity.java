// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import android.os.Bundle;
import android.os.Process;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * One title's runtime, in the ":title" process. SDL calls nk_android_title_main() in the title's
 * library (android/app/src/main/cpp/nk_android_title.c), which runs the runtime's main() with the
 * launcher's arguments and ends the process when the run ends, as a desktop child process ends.
 *
 * <p>Extras: library (a bare name such as nk_title_showcase-scene-v1 or a path in the native
 * library folder for a title the APK ships; the absolute path of a title the player built, in the
 * app's files folder), args (String[]), env (String[] of KEY=VALUE), cwd, log. TitleLibrary decides
 * which libraries may load; any other refuses the launch with exit status {@link #EXIT_REFUSED}.
 */
public class TitleActivity extends SDLActivity {
    static final String EXTRA_LIBRARY = "library";
    static final String EXTRA_ARGS = "args";
    static final String EXTRA_ENV = "env";
    static final String EXTRA_CWD = "cwd";
    static final String EXTRA_LOG = "log";
    /** The exit status of a refused library: what a POSIX shell gives a command it cannot run. */
    static final int EXIT_REFUSED = 126;

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
                if (eq > 0) NakagawaHost.setenv(entry.substring(0, eq), entry.substring(eq + 1));
            }
        }
        if (library != null) {
            TitleLibrary.Choice choice = TitleLibrary.resolve(
                    library, getApplicationInfo().nativeLibraryDir, getFilesDir().getPath());
            if (choice == null) {
                refuse(library, env);   // does not return: the process ends
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
    }

    /**
     * Ends this process without loading anything, as a desktop child ends when its program cannot
     * run: the status goes to NK_ANDROID_EXIT_FILE, which the launcher reads
     * (nk_platform_is_process_running in src/core/nk_platform_posix.c).
     */
    private void refuse(String library, String[] env) {
        Log.e(NakagawaHost.TAG, "TITLE_ACTIVITY refused library " + library
                + ": not a library the APK ships nor one built in the app's files folder");
        String exitFile = null;
        if (env != null) {
            for (String entry : env) {
                if (entry.startsWith("NK_ANDROID_EXIT_FILE=")) exitFile = entry.substring(21);
            }
        }
        if (exitFile != null && !exitFile.isEmpty()) {
            // A temporary name and a rename, as nk_android_title.c writes it.
            File temp = new File(exitFile + ".tmp");
            try (FileOutputStream out = new FileOutputStream(temp)) {
                out.write((EXIT_REFUSED + "\n").getBytes(StandardCharsets.US_ASCII));
            } catch (IOException e) {
                Log.e(NakagawaHost.TAG, "TITLE_ACTIVITY could not record the exit status: " + e);
            }
            if (!temp.renameTo(new File(exitFile))) {
                Log.e(NakagawaHost.TAG, "TITLE_ACTIVITY could not record the exit status");
            }
        }
        Process.killProcess(Process.myPid());
        System.exit(EXIT_REFUSED);
    }

    /** SDL3 always; a library the APK ships by name, while SDL dlopen()s a built one by path. */
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
