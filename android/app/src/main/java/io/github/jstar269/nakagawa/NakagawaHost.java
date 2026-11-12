// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileChannel;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * What the native code needs from Android before it starts, shared by both activities.
 *
 * <p>The native host layer (src/core/nk_platform_posix.c, Android sections) reads three
 * environment variables: NK_ANDROID_DATA_DIR, the app-private folder that holds the player's
 * data, settings, caches, logs and saves; NK_ANDROID_APP_DIR, the folder the APK's bundled files
 * (the "app" asset tree: UI fonts, bundled titles) are unpacked to, which becomes the working
 * directory; and NK_ANDROID_NATIVE_LIB_DIR, where the title runtime libraries are.
 *
 * <p>An APK built with -Pnk.toolchain also carries the on-device C toolchain (#360): its programs
 * (libnk_clang.so, libnk_ld.so, libnk_make.so, libnk_python.so) are in the native library folder,
 * and its archives in assets/toolchain/ are unpacked to toolchain/ in the files folder:
 * sysroot/, clang-resource/ (clang's -resource-dir) and python/ (Python's standard library).
 * NK_ANDROID_TOOLCHAIN_DIR names that folder, PYTHONHOME the python/ inside it and TMPDIR the
 * app's cache folder, which every program the app starts inherits. They are set only once the
 * unpack is complete; after a failed one they are absent, which means "no usable toolchain", and
 * a consumer must not guess the folder instead. The unpack (about 4,300 files) runs on a worker
 * thread (ToolchainArchive.Install), never on the UI thread: the launcher shows its progress,
 * or its failure with a way to retry, and starts the player once it is complete.
 *
 * <p>The bundled files are unpacked into the app's files folder itself: SDL_GetBasePath() is
 * "./" on Android, and SDL opens a relative path against the files folder
 * (SDL_GetAndroidInternalStoragePath), while plain fopen() resolves it against the working
 * directory. With both the same folder, the two agree.
 */
final class NakagawaHost {
    static final String TAG = "nakagawa";

    private NakagawaHost() {}

    /** The user data folder. Every host names it Nakagawa, with the same layout inside. */
    static File dataDir(Context context) {
        return new File(context.getFilesDir(), "Nakagawa");
    }

    static File appDir(Context context) {
        return context.getFilesDir();
    }

    /** Every variable set here, for {@link #nativeEnvironment}. */
    private static final java.util.LinkedHashMap<String, String> sEnvironment =
        new java.util.LinkedHashMap<>();

    static synchronized void setenv(String name, String value) {
        sEnvironment.put(name, value);
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException e) {
            Log.e(TAG, "setenv " + name + " failed", e);
        }
    }

    /**
     * The variables set through {@link #setenv}, as KEY=VALUE, read by the native libraries when
     * they load (JNI_OnLoad in src/core/nk_platform_posix.c). Os.setenv changes the environment
     * of the libc the app process runs on; native code that runs on another one, as an arm64
     * library does under a device's ARM translation layer, does not see it, so the native side
     * also takes the values from here.
     */
    static synchronized String[] nativeEnvironment() {
        String[] out = new String[sEnvironment.size()];
        int i = 0;
        for (java.util.Map.Entry<String, String> e : sEnvironment.entrySet()) {
            out[i++] = e.getKey() + "=" + e.getValue();
        }
        return out;
    }

    /**
     * Sets the environment and unpacks the bundled app files when this APK version has not yet.
     * The toolchain is not unpacked here, on the UI thread: the launcher starts that
     * ({@link #startToolchain}) and its variables are set once it is complete.
     */
    static void prepare(Context context) {
        File data = dataDir(context);
        File app = appDir(context);
        if (!data.isDirectory() && !data.mkdirs()) Log.e(TAG, "cannot create " + data);
        setenv("NK_ANDROID_DATA_DIR", data.getAbsolutePath());
        setenv("NK_ANDROID_APP_DIR", app.getAbsolutePath());
        setenv("NK_ANDROID_NATIVE_LIB_DIR", context.getApplicationInfo().nativeLibraryDir);
        String apk = apkStamp(context);
        String stamp = apk != null ? apk : "unknown";
        File stampFile = new File(app, ".unpacked");
        boolean bundlesToolchain;
        // The main and :title processes both get here: the lock keeps a second one from deleting
        // or unpacking while the first does, and it then finds the first one's stamp.
        try (RandomAccessFile lockFile = new RandomAccessFile(new File(app, ".unpack.lock"), "rw");
             FileChannel channel = lockFile.getChannel()) {
            channel.lock();  // released when the channel closes
            bundlesToolchain = hasAssets(context.getAssets(), "toolchain");
            if (!stamp.equals(ToolchainArchive.readText(stampFile))) {
                // Replace only what the APK bundles (assets/, titles/), never the user's data.
                String[] bundled = context.getAssets().list("app");
                if (bundled != null) {
                    for (String child : bundled) {
                        ToolchainArchive.deleteTree(new File(app, child));
                        copyAssetTree(context.getAssets(), "app/" + child, new File(app, child));
                    }
                }
                // A toolchain this APK no longer carries goes, its stamp first; one it carries
                // has its own stamp and is replaced by the launcher's unpack.
                if (!bundlesToolchain) {
                    ToolchainArchive.deleteTree(new File(app, TOOLCHAIN_STAMP));
                    ToolchainArchive.deleteTree(new File(app, "toolchain"));
                }
                // Written last, after every file is synced (copyAssetTree): without it, the next
                // start deletes and copies the bundled files again.
                ToolchainArchive.writeText(stampFile, stamp);
                Log.i(TAG, "BUNDLED_FILES_UNPACKED dir=" + app + " stamp=" + stamp);
            }
        } catch (IOException e) {
            Log.e(TAG, "BUNDLED_FILES_UNPACK_FAILED dir=" + app, e);
            return;
        }
        synchronized (NakagawaHost.class) {
            sBundlesToolchain = bundlesToolchain;
        }
        // Only after a complete unpack, by an earlier start with the same stamp; otherwise the
        // variables stay unset, so nothing is pointed at a partial toolchain/.
        if (bundlesToolchain && toolchain(context).isComplete()) setToolchainEnvironment(context);
    }

    /** The APK's version and install time, or null when they cannot be read. */
    private static String apkStamp(Context context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return info.getLongVersionCode() + "-" + info.lastUpdateTime;
        } catch (Exception e) {
            Log.e(TAG, "cannot read the app's version", e);
            return null;
        }
    }

    /** assets/toolchain/'s archives and the folder under toolchain/ each is unpacked to. */
    private static final String[][] TOOLCHAIN_ARCHIVES = {
        {"sysroot.tar", "sysroot"},
        {"clang-resource.tar", "clang-resource"},
        {"python-stdlib.tar", "python"},
    };
    /** In the files folder: the toolchain unpack's stamp, written last, and its lock. */
    private static final String TOOLCHAIN_STAMP = ".toolchain-unpacked";
    private static final String TOOLCHAIN_LOCK = ".toolchain.lock";

    /** Whether this APK bundles a toolchain, as the last {@link #prepare} found. */
    private static boolean sBundlesToolchain;
    /** This process's toolchain unpack: one, so an activity created again finds its run. */
    private static ToolchainArchive.Install sToolchain;
    private static ExecutorService sToolchainExecutor;

    private static synchronized ToolchainArchive.Install toolchain(Context context) {
        if (sToolchain == null) {
            AssetManager assets = context.getApplicationContext().getAssets();
            File app = appDir(context);
            sToolchain = new ToolchainArchive.Install(
                name -> new BufferedInputStream(assets.open("toolchain/" + name), 1 << 16),
                TOOLCHAIN_ARCHIVES, new File(app, "toolchain"), new File(app, TOOLCHAIN_STAMP),
                new File(app, TOOLCHAIN_LOCK), apkStamp(context));
        }
        return sToolchain;
    }

    /**
     * Starts the toolchain unpack on a worker thread when this APK bundles a toolchain that is not
     * unpacked yet. Returns that unpack, for the caller to show and to wait on before anything
     * uses the toolchain, or null when there is nothing to wait for (none bundled, complete, or
     * skipped in this process). A run already going on, or one that failed, is returned as it is:
     * {@link #retryToolchain} starts a failed one again, {@link #skipToolchain} gives up on it.
     * Only the launcher calls it; a title's runtime does not use the toolchain.
     */
    static synchronized ToolchainArchive.Install startToolchain(Context context) {
        if (!sBundlesToolchain) return null;
        ToolchainArchive.Install install = toolchain(context);
        ToolchainArchive.Status.State state = install.status().state;
        if (state == ToolchainArchive.Status.State.SKIPPED) return null;
        if (state == ToolchainArchive.Status.State.IDLE) {
            if (install.isComplete()) return null;
            install.start(toolchainExecutor(install));
        }
        return install;
    }

    /** Starts a failed toolchain unpack again; returns whether it started. */
    static synchronized boolean retryToolchain(ToolchainArchive.Install install) {
        return install.start(toolchainExecutor(install));
    }

    /**
     * Goes on without the toolchain after a failed unpack: the player starts with
     * NK_ANDROID_TOOLCHAIN_DIR, PYTHONHOME and TMPDIR unset, so building on the device is
     * unavailable, and the next start unpacks again. Returns whether the unpack had failed.
     */
    static boolean skipToolchain(ToolchainArchive.Install install) {
        boolean skipped = install.skip();
        if (skipped) {
            Log.w(TAG, "TOOLCHAIN_UNPACK_SKIPPED: the player starts without the build tools; building on the"
                + " device is unavailable until a later start unpacks them");
        }
        return skipped;
    }

    /** One worker thread, which logs each run's outcome. */
    private static Executor toolchainExecutor(ToolchainArchive.Install install) {
        if (sToolchainExecutor == null) {
            sToolchainExecutor = Executors.newSingleThreadExecutor(task -> new Thread(task, "nk-toolchain-unpack"));
        }
        ExecutorService executor = sToolchainExecutor;
        return task -> executor.execute(() -> {
            Log.i(TAG, "TOOLCHAIN_UNPACK_STARTED");
            task.run();
            ToolchainArchive.Status status = install.status();
            if (status.state == ToolchainArchive.Status.State.DONE) {
                Log.i(TAG, "TOOLCHAIN_UNPACKED files=" + status.done);
            } else {
                Log.e(TAG, "TOOLCHAIN_UNPACK_FAILED " + status);
            }
        });
    }

    /** Only once the unpack is complete: by an earlier start (prepare) or by this one's run. */
    static void setToolchainEnvironment(Context context) {
        File toolchain = new File(appDir(context), "toolchain");
        setenv("NK_ANDROID_TOOLCHAIN_DIR", toolchain.getAbsolutePath());
        setenv("PYTHONHOME", new File(toolchain, "python").getAbsolutePath());
        // clang and Python write temporary files; an app may write only its own folders.
        setenv("TMPDIR", context.getCacheDir().getAbsolutePath());
    }

    /** Whether the APK has assets under {@code path}; a failed listing is a failure, not "no". */
    private static boolean hasAssets(AssetManager assets, String path) throws IOException {
        String[] children = assets.list(path);
        return children != null && children.length > 0;
    }

    private static void copyAssetTree(AssetManager assets, String path, File target) throws IOException {
        String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                throw new IOException("cannot create " + parent);
            }
            try (InputStream in = assets.open(path); FileOutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[1 << 16];
                int n;
                while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                out.getFD().sync();  // on storage before the stamp says it is there
            }
            return;
        }
        if (!target.isDirectory() && !target.mkdirs()) throw new IOException("cannot create " + target);
        for (String child : children) copyAssetTree(assets, path + "/" + child, new File(target, child));
    }
}
