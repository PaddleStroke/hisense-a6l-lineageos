// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;
import android.view.Display;

import java.lang.reflect.Method;

/**
 * Stock behaviour (DisplayPowerController: screen_brightness_epd / screen_off_timeout_epd): the e-ink keeps its own
 * brightness (= frontlight) and screen timeout. When a6l_dualux reports a screen change, save the slider value and the
 * timeout of the screen we leave and restore the ones of the screen we enter. The slider always drives the active
 * screen (a6l_dualux routes it to the frontlight), this only makes each screen remember its own level.
 * Polls vendor.dualux.state every 500 ms while the display is on (no property change callbacks across partitions).
 */
final class PerScreenMemory {
    private static PerScreenMemory sInstance;
    private final Context mCtx;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final SharedPreferences mPrefs;
    private String mLastScreen;
    private boolean mStarted, mPolling;
    private String mPrepareRequest = "";
    private final AppearanceGate mAppearanceGate = new AppearanceGate();
    private final WindowPropertyNotifier mWindowPropertyNotifier = new WindowPropertyNotifier();
    private long mAppearanceStarted;
    private int mAppearanceStages = -1;
    private String mAppearanceReason = "";

    static synchronized PerScreenMemory get(Context c) {
        if (sInstance == null) sInstance = new PerScreenMemory(c.getApplicationContext());
        return sInstance;
    }

    private PerScreenMemory(Context c) {
        mCtx = c.createDeviceProtectedStorageContext();
        mPrefs = mCtx.getSharedPreferences("per_screen", Context.MODE_PRIVATE);
    }

    /* Cheap awake request detection is separate from brightness/timeout work.
     * The appearance gate is stepped at 100 ms only for an outstanding request
     * (or its cancellation), never an idle full settings/Binder refresh. */
    private final Runnable mPreparePoll = new Runnable() {
        @Override public void run() {
            if (!mPolling) return;
            String request = Dualux.get(Dualux.PREPARE, "");
            if (!request.isEmpty() || !mPrepareRequest.isEmpty()) checkAppearance(request);
            mPrepareRequest = request;
            if (mPolling) mHandler.postDelayed(this, 100);
        }
    };

    private final Runnable mPoll = new Runnable() {
        @Override public void run() {
            if (!mPolling) return;
            checkBookkeeping();
            if (mPolling) mHandler.postDelayed(this, 500);
        }
    };

    void start() {
        if (mStarted) return;
        mStarted = true;
        Dualux.set(Dualux.P_THEME_SYNC, PerScreenAppearance.get(mCtx).available() ? "1" : "0");
        IntentFilter f = new IntentFilter(Intent.ACTION_SCREEN_ON);
        f.addAction(Intent.ACTION_SCREEN_OFF);
        mCtx.registerReceiver(new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) { setPolling(Intent.ACTION_SCREEN_ON.equals(i.getAction())); }
        }, f, Context.RECEIVER_NOT_EXPORTED);
        PowerManager pm = mCtx.getSystemService(PowerManager.class);
        mLastScreen = screen();
        setPolling(pm == null || pm.isInteractive());
    }

    private void setPolling(boolean on) {
        if (on == mPolling) return;
        mPolling = on;
        mHandler.removeCallbacks(mPreparePoll);
        mHandler.removeCallbacks(mPoll);
        if (on) {
            mHandler.post(mPreparePoll);
            mHandler.post(mPoll);
        }
    }

    private static String screen() { return Dualux.isEink() ? "eink" : "lcd"; }

    private void checkAppearance(String request) {
        final PerScreenAppearance appearance = PerScreenAppearance.get(mCtx);
        mAppearanceGate.step(request, SystemClock.uptimeMillis(), new AppearanceGate.Backend() {
            @Override public boolean apply(String request, boolean eink) {
                if (!request.equals(Dualux.get(Dualux.PREPARE, ""))) return false;
                mAppearanceStarted = SystemClock.elapsedRealtime(); mAppearanceStages = -1;
                Log.i(Dualux.TAG, "appearance " + request + " stage apply start_ms=" + mAppearanceStarted);
                // WMS receives the atomic target before the theme Binder call.
                Dualux.set(Dualux.APPEARANCE, request);
                notifyWindowProperties(request, false);
                boolean applied = appearance.update(eink);
                Log.i(Dualux.TAG, "appearance " + request + " stage applied=" + applied
                        + " elapsed_ms=" + (SystemClock.elapsedRealtime() - mAppearanceStarted));
                return applied;
            }
            @Override public boolean ready(String request, boolean eink) {
                if (!currentAppearance(request)) return false;
                boolean theme = appearance.ready(eink);
                boolean wallpaper = request.equals(Dualux.get(Dualux.WALLPAPER_READY, ""));
                boolean frame = request.equals(Dualux.get(Dualux.FRAME_READY, ""));
                int stages = (theme ? 1 : 0) | (wallpaper ? 2 : 0) | (frame ? 4 : 0);
                String reason = appearance.readyReason();
                if (stages != mAppearanceStages || !reason.equals(mAppearanceReason)) {
                    mAppearanceStages = stages;
                    mAppearanceReason = reason;
                    Log.i(Dualux.TAG, "appearance " + request + " stage theme=" + theme
                            + " wallpaper=" + wallpaper + " frame=" + frame + " reason=" + reason + " elapsed_ms="
                            + (SystemClock.elapsedRealtime() - mAppearanceStarted));
                }
                if (!theme || !currentAppearance(request)) return false;
                if (!request.equals(Dualux.get(Dualux.THEME_READY, ""))) {
                    Dualux.set(Dualux.THEME_READY, request);
                }
                notifyWindowProperties(request, true);
                return Dualux.getInt(Dualux.WALLPAPER_SYNC, 0) == 0
                        || (wallpaper && frame);
            }
            @Override public void acknowledge(String request) {
                if (!currentAppearance(request)) return;
                Dualux.set(Dualux.P_READY, request);
                Log.i(Dualux.TAG, "appearance ready " + request);
            }
        });
    }

    private boolean currentAppearance(String request) {
        return request.equals(Dualux.get(Dualux.PREPARE, ""))
                && request.equals(Dualux.get(Dualux.APPEARANCE, ""));
    }

    private void notifyWindowProperties(String request, boolean themeReady) {
        mWindowPropertyNotifier.published(request, themeReady, new WindowPropertyNotifier.Backend() {
            @Override public boolean systemUid() { return Process.myUid() == Process.SYSTEM_UID; }
            @Override public String prepare() { return Dualux.get(Dualux.PREPARE, ""); }
            @Override public String appearance() { return Dualux.get(Dualux.APPEARANCE, ""); }
            @Override public String themeReady() { return Dualux.get(Dualux.THEME_READY, ""); }
            @Override public void notifyWindow(String pair, boolean ready) {
                // Unlike local reportSyspropChanged(), this dispatches callbacks in system_server.
                Parcel data = null;
                try {
                    IBinder window = ServiceManager.checkService(Context.WINDOW_SERVICE);
                    if (window == null || !WindowPropertyNotifier.current(pair, ready, this)) return;
                    data = Parcel.obtain();
                    boolean queued = window.transact(IBinder.SYSPROPS_TRANSACTION, data, null,
                            IBinder.FLAG_ONEWAY);
                    Log.i(Dualux.TAG, "appearance " + pair + " WM notify phase="
                            + (ready ? "theme" : "target") + " queued=" + queued
                            + " elapsed_ms=" + (SystemClock.elapsedRealtime() - mAppearanceStarted));
                } catch (RemoteException | RuntimeException ex) {
                    Log.w(Dualux.TAG, "appearance " + pair + " WM notify failed", ex);
                } finally {
                    if (data != null) data.recycle();
                }
            }
        });
    }

    private void checkBookkeeping() {
        boolean running = Dualux.daemonRunning();
        PerScreenAppearance.get(mCtx).update(running && Dualux.isEink());
        if (!running) return;
        String now = screen();
        if (now.equals(mLastScreen)) return;
        String old = mLastScreen;
        mLastScreen = now;
        if (Dualux.getInt(Dualux.P_PER_SCREEN, 1) == 0) return;
        ContentResolver cr = mCtx.getContentResolver();
        boolean auto = Settings.System.getInt(cr, Settings.System.SCREEN_BRIGHTNESS_MODE, 0) != 0;
        SharedPreferences.Editor e = mPrefs.edit();
        float b = getBrightness();
        if (!auto && b >= 0) e.putFloat("brightness_" + old, b);
        e.putInt("timeout_" + old, Settings.System.getInt(cr, Settings.System.SCREEN_OFF_TIMEOUT, 60000));
        e.apply();
        if (!auto && mPrefs.contains("brightness_" + now)) setBrightness(mPrefs.getFloat("brightness_" + now, 0.5f));
        int def = "eink".equals(now) ? 120000 : -1;   // first time on the e-ink: 2 min (e-paper costs nothing to show)
        int to = mPrefs.getInt("timeout_" + now, def);
        if (to > 0) Settings.System.putInt(cr, Settings.System.SCREEN_OFF_TIMEOUT, to);
        Log.i(Dualux.TAG, "screen " + old + " -> " + now + ": brightness " + b + " saved, timeout " + to);
    }

    // DisplayManager.getBrightness/setBrightness(int, float) are @hide/@SystemApi (CONTROL_DISPLAY_BRIGHTNESS); fall back to
    // the legacy 0..255 Settings.System value when reflection fails.
    private float getBrightness() {
        try {
            DisplayManager dm = mCtx.getSystemService(DisplayManager.class);
            Method m = DisplayManager.class.getMethod("getBrightness", int.class);
            return (float) m.invoke(dm, Display.DEFAULT_DISPLAY);
        } catch (ReflectiveOperationException | RuntimeException ex) {
            int v = Settings.System.getInt(mCtx.getContentResolver(), Settings.System.SCREEN_BRIGHTNESS, -1);
            return v < 0 ? -1 : v / 255f;
        }
    }

    private void setBrightness(float b) {
        try {
            DisplayManager dm = mCtx.getSystemService(DisplayManager.class);
            Method m = DisplayManager.class.getMethod("setBrightness", int.class, float.class);
            m.invoke(dm, Display.DEFAULT_DISPLAY, b);
        } catch (ReflectiveOperationException | RuntimeException ex) {
            Settings.System.putInt(mCtx.getContentResolver(), Settings.System.SCREEN_BRIGHTNESS, Math.round(b * 255));
        }
    }
}
