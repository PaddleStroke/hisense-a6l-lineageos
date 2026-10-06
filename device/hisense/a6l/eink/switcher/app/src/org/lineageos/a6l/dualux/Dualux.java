// SPDX-License-Identifier: Apache-2.0
// Hisense A6L display switcher (agent dualux, docs/dualux-20260925.md): property contract with the vendor daemon
// a6l_dualux and the e-ink mirror. Only properties cross the system/vendor boundary (no sockets, no binder).
package org.lineageos.a6l.dualux;

import android.os.SystemClock;
import android.util.Log;

import java.lang.reflect.Method;

final class Dualux {
    static final String TAG = "A6LDualux";
    // written by a6l_dualux (vendor_dualux_prop, read-only here)
    static final String P_STATE = "vendor.dualux.state";            // lcd | eink | eink-asleep
    static final String PREPARE = "vendor.dualux.prepare"; // daemon: token + target
    static final String P_READY = "sys.a6l.dualux.ready";
    static final String P_THEME_SYNC = "sys.a6l.dualux.theme_sync";
    static final String APPEARANCE = "sys.a6l.dualux.appearance";
    static final String WALLPAPER_READY = "sys.a6l.dualux.wallpaper_ready";
    static final String WALLPAPER_SYNC = "sys.a6l.dualux.wallpaper_sync";
    static final String THEME_READY = "sys.a6l.dualux.theme_ready";
    static final String FRAME_READY = "sys.a6l.dualux.frame_ready";
    static final String NO_ANIMATIONS = "sys.a6l.eink.no_animations";
    static final String GESTURE_SLOP = "sys.a6l.eink.gesture_slop";
    // written here (vendor_dualux_ctl_prop)
    static final String P_REQ = "sys.a6l.dualux.req";                // "<seq> eink|lcd|toggle|clear"
    static final String P_REFRESH = "persist.sys.a6l.eink.refresh";  // auto|quality|partial|fast|fastest (mirror, live)
    static final String P_CLEAR_EVERY = "persist.sys.a6l.eink.clear_every"; // forced page refresh every N updates (0 = never)
    static final String P_CONTRAST = "persist.sys.a6l.eink.contrast";   // 0..100 (mirror, live)
    static final String P_FL_ENABLE = "persist.sys.a6l.dualux.fl_enable";
    static final String P_FL_MAX = "persist.sys.a6l.dualux.fl_max_pct";
    static final String P_FL_GAMMA = "persist.sys.a6l.dualux.fl_gamma";
    static final String P_EKEY = "persist.sys.a6l.dualux.eink_key";  // sleep | clear
    static final String P_MIRROR_LCD = "persist.sys.a6l.dualux.mirror_in_lcd";
    static final String P_PER_SCREEN = "persist.sys.a6l.dualux.per_screen";
    // eink-lockscreen: e-ink lock screen (vendor a6l_einklock), set from Settings > Display > E-ink
    static final String P_LOCK = "persist.sys.a6l.eink.lock";               // 1 | 0 (default 1)
    static final String P_LOCK_BG = "persist.sys.a6l.eink.lock_bg";         // white | black | image | lcd
    static final String P_LOCK_CLOCK = "persist.sys.a6l.eink.lock_clock";   // 1 | 0: time + date, minute RTC wake-ups
    static final String P_LOCK_BATTERY = "persist.sys.a6l.eink.lock_battery"; // 1 | 0
    static final String P_LOCK_24H = "persist.sys.a6l.eink.lock_24h";       // mirrored from Android by LockSync
    static final int DEFAULT_CLEAR_EVERY = 10;   // a6l_eink.rc default (--clear-every 10)

    private static Method sGet, sSet;

    private Dualux() {}

    static String get(String key, String def) {
        try {
            if (sGet == null) sGet = Class.forName("android.os.SystemProperties").getMethod("get", String.class, String.class);
            return (String) sGet.invoke(null, key, def);
        } catch (ReflectiveOperationException e) {
            Log.w(TAG, "get " + key, e);
            return def;
        }
    }

    static void set(String key, String value) {
        try {
            if (sSet == null) sSet = Class.forName("android.os.SystemProperties").getMethod("set", String.class, String.class);
            sSet.invoke(null, key, value);
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.e(TAG, "set " + key + "=" + value + " refused (SELinux?)", e);
        }
    }

    static int getInt(String key, int def) {
        try { return Integer.parseInt(get(key, Integer.toString(def)).trim()); } catch (NumberFormatException e) { return def; }
    }

    static String state() { return get(P_STATE, ""); }
    static boolean daemonRunning() { return !state().isEmpty(); }
    static boolean isEink() { return state().startsWith("eink"); }

    static void request(String cmd) {
        set(P_REQ, SystemClock.elapsedRealtime() + " " + cmd);
        Log.i(TAG, "request " + cmd);
    }

    static String refreshMode() {
        // persist.vendor.eink.reading is vendor_internal (system_app may not read it): unset/unknown = stock, which is
        // also what the mirror applies when persist.sys.a6l.eink.refresh is unset and .reading is 0 (the build default)
        return RefreshModes.normalize(get(P_REFRESH, ""));
    }

    static String nextMode(String m) { return RefreshModes.next(m); }

    /** Short, translated mode name for the Quick Settings subtitle. */
    static int shortLabel(String m) {
        switch (m) {
            case "stock": return R.string.short_stock;
            case "quality": return R.string.short_quality;
            case "partial": return R.string.short_partial;
            case "fast": return R.string.short_fast;
            case "fastest": return R.string.short_fastest;
            default: return R.string.short_auto;
        }
    }
}
