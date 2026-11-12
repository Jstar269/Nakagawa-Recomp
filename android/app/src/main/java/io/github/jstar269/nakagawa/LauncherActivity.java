// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The launcher: SDL runs the player's main() from libmain.so (src/player/main.c) on its thread.
 * When the player starts a title, the native host layer calls {@link #startTitle}.
 *
 * <p>In an APK that bundles the on-device toolchain, the first start after an install or update
 * unpacks it on a worker thread (NakagawaHost.startToolchain). Meanwhile this activity shows the
 * files done of the total over SDL's surface, and SDL's thread waits in {@link #main} until the
 * unpack is complete, so the player never starts with a partial toolchain. Then the toolchain's
 * variables are set and handed to the native side (nativeImportEnvironment) before the player
 * starts. A failure shows its reason, Try again, and Continue without the build tools, which
 * starts the player with the variables unset; the next start unpacks again.
 */
public class LauncherActivity extends SDLActivity {
    private static volatile LauncherActivity sCurrent;
    /** The toolchain unpack the player waits for, or null; static, as SDL's thread outlives an activity. */
    private static volatile ToolchainArchive.Install sToolchain;
    private static final Object sWaitLock = new Object();
    /** SDL's thread while it waits in {@link #main}, so {@link #onDestroy} can end the wait. */
    private static Thread sWaiter;
    /** Set by {@link #onDestroy}, for a main() that had not begun to wait yet. */
    private static boolean sClosing;

    /**
     * Applies NakagawaHost's variables to the player library's own libc again
     * (src/core/nk_platform_posix.c). Its JNI_OnLoad did so when it loaded, before a toolchain
     * unpacked on this start had set its variables; under an ARM translation layer the library's
     * libc is not the one Os.setenv changes.
     */
    private static native void nativeImportEnvironment();

    private UnpackPanel mUnpackPanel;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sCurrent = this;
        synchronized (sWaitLock) {
            sClosing = false;
        }
        NakagawaHost.prepare(this);
        super.onCreate(savedInstanceState);
        // Before onResume, where SDL starts its thread and so main().
        ToolchainArchive.Install install = NakagawaHost.startToolchain(this);
        sToolchain = install;
        if (install != null && mLayout != null) mUnpackPanel = new UnpackPanel(this, install);
    }

    @Override
    protected void onDestroy() {
        if (sCurrent == this) sCurrent = null;
        if (mUnpackPanel != null) mUnpackPanel.close();
        // SDL's onDestroy asks the player's main() to quit; a main() still waiting for the unpack
        // has not started it, so it is told here instead.
        synchronized (sWaitLock) {
            sClosing = true;
            if (sWaiter != null) sWaiter.interrupt();
        }
        super.onDestroy();
    }

    /**
     * On SDL's thread: waits for the toolchain unpack, if one is going on. Once it is complete,
     * sets the toolchain's variables, then hands them to the native side, then runs the player;
     * when the user went on without it, runs the player with them unset.
     */
    @Override
    protected void main() {
        ToolchainArchive.Install install = sToolchain;
        if (install != null) {
            boolean ended = false;
            synchronized (sWaitLock) {
                if (sClosing) ended = true;  // onDestroy came before this wait began
                sWaiter = Thread.currentThread();
            }
            try {
                if (!ended && !install.awaitReady(() -> NakagawaHost.setToolchainEnvironment(this),
                                                  LauncherActivity::nativeImportEnvironment)) {
                    Log.w(NakagawaHost.TAG, "TOOLCHAIN_UNAVAILABLE: the player starts without the build tools");
                }
            } catch (InterruptedException e) {
                ended = true;
            } finally {
                synchronized (sWaitLock) {
                    sWaiter = null;
                    // An interrupt that came after the wait ended still means the activity is going.
                    if (Thread.interrupted()) ended = true;
                }
            }
            if (ended) {
                Log.i(NakagawaHost.TAG, "TOOLCHAIN_WAIT_ENDED: the launcher closed before the unpack finished");
                return;
            }
        }
        super.main();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mUnpackPanel != null && mUnpackPanel.key(event)) return true;
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String getMainFunction() {
        return "main";
    }

    @Override
    protected String[] getArguments() {
        return new String[0];
    }

    /**
     * Starts one title's runtime in the ":title" process (TitleActivity). Called from native code
     * (nk_platform_spawn_process) with what a desktop launch passes to its child: the runtime's
     * library path in place of the executable, the arguments after argv[0], KEY=VALUE environment
     * entries, the working directory and the run-log path (either may be null).
     */
    public static boolean startTitle(String library, String[] args, String[] env, String cwd,
                                     String logPath) {
        LauncherActivity launcher = sCurrent;
        if (launcher == null) return false;
        Intent intent = new Intent(launcher, TitleActivity.class);
        intent.putExtra(TitleActivity.EXTRA_LIBRARY, library);
        intent.putExtra(TitleActivity.EXTRA_ARGS, args);
        intent.putExtra(TitleActivity.EXTRA_ENV, env);
        if (cwd != null) intent.putExtra(TitleActivity.EXTRA_CWD, cwd);
        if (logPath != null) intent.putExtra(TitleActivity.EXTRA_LOG, logPath);
        Log.i(NakagawaHost.TAG, "TITLE_START library=" + library + " args=" + args.length);
        launcher.runOnUiThread(() -> launcher.startActivity(intent));
        return true;
    }

    /**
     * The unpack's progress over SDL's surface: a bar and "N of M files". On a failure: its
     * reason, that building on the device is unavailable until an unpack succeeds, and two
     * buttons, Try again and Continue without the build tools. A touch presses either; the arrow
     * keys, Tab or a controller's D-pad move between them, and Enter or the A button presses the
     * focused one. Light text on a near-black ground, as the player's own screens
     * (ACCESSIBILITY.md). It goes away when the unpack is complete or skipped.
     */
    private static final class UnpackPanel {
        private final LauncherActivity activity;
        private final ToolchainArchive.Install install;
        private final LinearLayout panel;
        private final ProgressBar bar;
        private final TextView detail;
        private final LinearLayout actions;
        private final Button retry;
        private final Button skip;
        private final AtomicBoolean posted = new AtomicBoolean();
        private boolean closed;

        UnpackPanel(LauncherActivity activity, ToolchainArchive.Install install) {
            this.activity = activity;
            this.install = install;
            int pad = dp(32);
            panel = new LinearLayout(activity);
            panel.setOrientation(LinearLayout.VERTICAL);
            panel.setGravity(Gravity.CENTER);
            panel.setBackgroundColor(Color.rgb(0x10, 0x10, 0x14));
            panel.setPadding(pad, pad, pad, pad);
            panel.setClickable(true);  // the surface below takes no touches meanwhile
            TextView title = text(activity.getString(R.string.toolchain_unpack_title), 22);
            bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
            detail = text("", 16);
            retry = new Button(activity);
            retry.setText(R.string.toolchain_unpack_retry);
            retry.setOnClickListener(v -> NakagawaHost.retryToolchain(install));
            skip = new Button(activity);
            skip.setText(R.string.toolchain_unpack_skip);
            skip.setOnClickListener(v -> NakagawaHost.skipToolchain(install));
            actions = new LinearLayout(activity);
            actions.setOrientation(LinearLayout.HORIZONTAL);
            actions.setGravity(Gravity.CENTER);
            actions.addView(retry);
            actions.addView(skip);
            panel.addView(title);
            panel.addView(bar, new LinearLayout.LayoutParams(dp(360), ViewGroup.LayoutParams.WRAP_CONTENT));
            panel.addView(detail);
            panel.addView(actions);
            mLayout.addView(panel, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            // Told on the worker thread, once per file: one redraw at a time is queued.
            install.setListener(status -> {
                if (posted.compareAndSet(false, true)) {
                    activity.runOnUiThread(() -> {
                        posted.set(false);
                        show(install.status());
                    });
                }
            });
            show(install.status());
        }

        private void show(ToolchainArchive.Status status) {
            if (closed) return;
            switch (status.state) {
                case DONE:
                case SKIPPED:
                    close();
                    break;
                case FAILED:
                    bar.setVisibility(View.GONE);
                    // Announced once, on the failure, not on each of the thousands of progress
                    // steps; the reason and the note are one text, so both are read.
                    detail.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
                    detail.setText(activity.getString(R.string.toolchain_unpack_failed, status.error) + "\n\n"
                        + activity.getString(R.string.toolchain_unpack_unavailable));
                    actions.setVisibility(View.VISIBLE);
                    retry.requestFocus();
                    break;
                default:  // IDLE before a retry starts, RUNNING
                    bar.setVisibility(View.VISIBLE);
                    detail.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_NONE);
                    actions.setVisibility(View.GONE);
                    bar.setIndeterminate(status.total == 0);
                    if (status.total == 0) {
                        detail.setText(R.string.toolchain_unpack_counting);
                    } else {
                        bar.setMax(status.total);
                        bar.setProgress(status.done);
                        detail.setText(activity.getString(R.string.toolchain_unpack_progress, status.done, status.total));
                    }
                    break;
            }
        }

        /**
         * While the failure's buttons show: a direction key or Tab moves the focus to the other
         * button, and a confirm key presses the focused one (Try again unless Continue has it).
         * SDL's surface can take the focus back, so the keys are handled here, not left to it.
         * Returns whether the key was used.
         */
        boolean key(KeyEvent event) {
            if (closed || actions.getVisibility() != View.VISIBLE) return false;
            switch (event.getKeyCode()) {
                case KeyEvent.KEYCODE_DPAD_LEFT:
                case KeyEvent.KEYCODE_DPAD_RIGHT:
                case KeyEvent.KEYCODE_DPAD_UP:
                case KeyEvent.KEYCODE_DPAD_DOWN:
                case KeyEvent.KEYCODE_TAB:
                    if (event.getAction() == KeyEvent.ACTION_DOWN) (retry.isFocused() ? skip : retry).requestFocus();
                    return true;
                case KeyEvent.KEYCODE_DPAD_CENTER:
                case KeyEvent.KEYCODE_ENTER:
                case KeyEvent.KEYCODE_NUMPAD_ENTER:
                case KeyEvent.KEYCODE_SPACE:
                case KeyEvent.KEYCODE_BUTTON_A:
                    if (event.getAction() == KeyEvent.ACTION_UP) (skip.isFocused() ? skip : retry).performClick();
                    return true;
                default:
                    return false;
            }
        }

        void close() {
            if (closed) return;
            closed = true;
            install.setListener(null);
            ViewGroup parent = (ViewGroup) panel.getParent();
            if (parent != null) parent.removeView(panel);
            if (activity.mUnpackPanel == this) activity.mUnpackPanel = null;
        }

        private TextView text(String value, int sp) {
            TextView view = new TextView(activity);
            view.setText(value);
            view.setTextColor(Color.rgb(0xF0, 0xF0, 0xF2));
            view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
            view.setGravity(Gravity.CENTER);
            view.setPadding(0, dp(8), 0, dp(8));
            return view;
        }

        private int dp(int value) {
            return Math.round(value * activity.getResources().getDisplayMetrics().density);
        }
    }
}
