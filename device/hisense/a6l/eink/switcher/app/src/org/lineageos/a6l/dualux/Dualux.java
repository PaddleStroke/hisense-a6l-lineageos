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
    // written here (vendor_dualux_ctl_prop)
    static final String P_REQ = "sys.a6l.dualux.req";                // "<seq> eink|lcd|toggle|clear"
    static final String P_REFRESH = "persist.sys.a6l.eink.refresh";  // auto|quality|partial|fast|fastest (mirror, live)
    static final String P_CLEAR_EVERY = "persist.sys.a6l.eink.clear_every"; // full clear every N clean updates (0 = never)
    static final String P_CONTRAST = "persist.sys.a6l.eink.contrast";   // 0..100 (mirror, live)
    static final String P_FL_ENABLE = "persist.sys.a6l.dualux.fl_enable";
    static final String P_FL_MAX = "persist.sys.a6l.dualux.fl_max_pct";
    static final String P_FL_GAMMA = "persist.sys.a6l.dualux.fl_gamma";
    static final String P_EKEY = "persist.sys.a6l.dualux.eink_key";  // sleep | clear
    static final String P_MIRROR_LCD = "persist.sys.a6l.dualux.mirror_in_lcd";
    static final String P_PER_SCREEN = "persist.sys.a6l.dualux.per_screen";
    static final String[] MODES = {"auto", "quality", "partial", "fast", "fastest"};
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
        String m = get(P_REFRESH, "");
        if (m.isEmpty()) m = "1".equals(get("persist.vendor.eink.reading", "0")) ? "partial" : "auto";
        return m;
    }

    static String nextMode(String m) {
        for (int i = 0; i < MODES.length; i++) if (MODES[i].equals(m)) return MODES[(i + 1) % MODES.length];
        return MODES[0];
    }

    static int modeLabel(String m) {
        switch (m) {
            case "quality": return R.string.mode_quality;
            case "partial": return R.string.mode_partial;
            case "fast": return R.string.mode_fast;
            case "fastest": return R.string.mode_fastest;
            default: return R.string.mode_auto;
        }
    }

    static String shortName(String m) {
        switch (m) {
            case "partial": return "Reading";
            default: return Character.toUpperCase(m.charAt(0)) + m.substring(1);
        }
    }
}
