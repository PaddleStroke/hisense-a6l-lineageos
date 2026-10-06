// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.ContentObserver;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.format.DateFormat;

/**
 * eink-lockscreen: Android state the vendor lock-screen daemon cannot read itself, mirrored into properties by this
 * persistent app: the 12/24-hour preference (Settings.System.TIME_12_24 -> persist.sys.a6l.eink.lock_24h) and, when the
 * background is "lcd", a re-export of the LCD wallpaper after it changes. Locale and time zone are read natively.
 */
final class LockSync {
    private static LockSync sInstance;
    private final Context mContext;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Runnable mExport = this::exportLcd;

    static synchronized void start(Context context) {
        if (sInstance == null) { sInstance = new LockSync(context.getApplicationContext()); sInstance.init(); }
    }

    private LockSync(Context context) { mContext = context; }

    private void init() {
        sync24h();
        mContext.getContentResolver().registerContentObserver(Settings.System.getUriFor(Settings.System.TIME_12_24), false,
                new ContentObserver(mHandler) { @Override public void onChange(boolean self) { sync24h(); } });
        IntentFilter f = new IntentFilter(Intent.ACTION_TIME_CHANGED);
        f.addAction(Intent.ACTION_LOCALE_CHANGED);
        f.addAction("android.intent.action.WALLPAPER_CHANGED");	/* Intent.ACTION_WALLPAPER_CHANGED (deprecated constant) */
        mContext.registerReceiver(new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) {
                sync24h();
                if ("android.intent.action.WALLPAPER_CHANGED".equals(i.getAction()) && "lcd".equals(Dualux.get(Dualux.P_LOCK_BG, ""))) {
                    mHandler.removeCallbacks(mExport); mHandler.postDelayed(mExport, 2000);
                }
            }
        }, f, Context.RECEIVER_NOT_EXPORTED);
    }

    private void sync24h() {
        String v = DateFormat.is24HourFormat(mContext) ? "1" : "0";
        if (!v.equals(Dualux.get(Dualux.P_LOCK_24H, ""))) Dualux.set(Dualux.P_LOCK_24H, v);
    }

    /** Settings chose "lcd" (or the wallpaper changed): export it off the main thread. */
    static void exportLcdAsync(Context context) {
        Context app = context.getApplicationContext();
        new Thread(() -> {
            if (!LockBackground.fromLcdWallpaper(app)) Dualux.set(Dualux.P_LOCK_BG, "lcd");	/* daemon falls back to white */
        }, "a6l-lock-lcd").start();
    }

    private void exportLcd() { exportLcdAsync(mContext); }
}
