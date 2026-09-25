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
import android.os.Looper;
import android.os.PowerManager;
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

    static synchronized PerScreenMemory get(Context c) {
        if (sInstance == null) sInstance = new PerScreenMemory(c.getApplicationContext());
        return sInstance;
    }

    private PerScreenMemory(Context c) {
        mCtx = c.createDeviceProtectedStorageContext();
        mPrefs = mCtx.getSharedPreferences("per_screen", Context.MODE_PRIVATE);
    }

    private final Runnable mPoll = new Runnable() {
        @Override public void run() {
            check();
            if (mPolling) mHandler.postDelayed(this, 500);
        }
    };

    void start() {
        if (mStarted) return;
        mStarted = true;
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
        mHandler.removeCallbacks(mPoll);
        if (on) mHandler.post(mPoll);
    }

    private static String screen() { return Dualux.isEink() ? "eink" : "lcd"; }

    private void check() {
        if (!Dualux.daemonRunning()) return;
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
