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
 * unpack is complete, so the player never starts with a partial toolchain. A failure shows its
 * reason and a Try again button; the player keeps waiting.
 */
public class LauncherActivity extends SDLActivity {
    private static volatile LauncherActivity sCurrent;
    /** The toolchain unpack the player waits for, or null; static, as SDL's thread outlives an activity. */
    private static volatile ToolchainArchive.Install sToolchain;
    private static final Object sWaitLock = new Object();
    /** SDL's thread while it waits in {@link #main}, so {@link #onDestroy} can end the wait. */
    private static Thread sWaiter;

    private UnpackPanel mUnpackPanel;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sCurrent = this;
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
            if (sWaiter != null) sWaiter.interrupt();
        }
        super.onDestroy();
    }

    /** On SDL's thread: waits for the toolchain unpack, if one is going on, then runs the player. */
    @Override
    protected void main() {
        ToolchainArchive.Install install = sToolchain;
        if (install != null) {
            synchronized (sWaitLock) {
                sWaiter = Thread.currentThread();
            }
            boolean ended = false;
            try {
                NakagawaHost.awaitToolchain(this, install);
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
     * The unpack's progress over SDL's surface: a bar and "N of M files", then on a failure its
     * reason and a Try again button that a touch, Enter or a controller's A button presses. Light
     * text on a near-black ground, as the player's own screens (ACCESSIBILITY.md). It goes away
     * when the unpack is complete.
     */
    private static final class UnpackPanel {
        private final LauncherActivity activity;
        private final ToolchainArchive.Install install;
        private final LinearLayout panel;
        private final ProgressBar bar;
        private final TextView detail;
        private final Button retry;
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
            detail.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
            retry = new Button(activity);
            retry.setText(R.string.toolchain_unpack_retry);
            retry.setVisibility(View.GONE);
            retry.setOnClickListener(v -> NakagawaHost.retryToolchain(install));
            panel.addView(title);
            panel.addView(bar, new LinearLayout.LayoutParams(dp(360), ViewGroup.LayoutParams.WRAP_CONTENT));
            panel.addView(detail);
            panel.addView(retry);
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
                    close();
                    break;
                case FAILED:
                    bar.setVisibility(View.GONE);
                    detail.setText(activity.getString(R.string.toolchain_unpack_failed, status.error));
                    retry.setVisibility(View.VISIBLE);
                    retry.requestFocus();
                    break;
                default:  // IDLE before a retry starts, RUNNING
                    bar.setVisibility(View.VISIBLE);
                    retry.setVisibility(View.GONE);
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

        /** Presses Try again for a confirm key while it shows; returns whether the key was used. */
        boolean key(KeyEvent event) {
            if (closed || retry.getVisibility() != View.VISIBLE) return false;
            switch (event.getKeyCode()) {
                case KeyEvent.KEYCODE_DPAD_CENTER:
                case KeyEvent.KEYCODE_ENTER:
                case KeyEvent.KEYCODE_NUMPAD_ENTER:
                case KeyEvent.KEYCODE_SPACE:
                case KeyEvent.KEYCODE_BUTTON_A:
                    break;
                default:
                    return false;
            }
            if (event.getAction() == KeyEvent.ACTION_UP) retry.performClick();
            return true;
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
