// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** Narrow public setting contract; callers cannot name arbitrary system properties. */
final class SettingsValues {
    static final String[] KEYS = {"refresh", "clear_every", "contrast", "fl_enable",
            "fl_max", "fl_gamma", "ekey", "mirror_lcd", "per_screen", "wallpaper",
            "lock", "lock_bg", "lock_clock", "lock_battery", "reader_sleep", "reader_apps"};
    private SettingsValues() { }

    static String property(String key) {
        if (key == null) throw new IllegalArgumentException("Missing setting");
        switch (key) {
            case "refresh": return "persist.sys.a6l.eink.refresh";
            case "clear_every": return "persist.sys.a6l.eink.clear_every";
            case "contrast": return "persist.sys.a6l.eink.contrast";
            case "fl_enable": return "persist.sys.a6l.dualux.fl_enable";
            case "fl_max": return "persist.sys.a6l.dualux.fl_max_pct";
            case "fl_gamma": return "persist.sys.a6l.dualux.fl_gamma";
            case "ekey": return "persist.sys.a6l.dualux.eink_key";
            case "mirror_lcd": return "persist.sys.a6l.dualux.mirror_in_lcd";
            case "per_screen": return "persist.sys.a6l.dualux.per_screen";
            case "wallpaper": return "persist.sys.a6l.eink.wallpaper";
            case "lock": return "persist.sys.a6l.eink.lock";
            case "lock_bg": return "persist.sys.a6l.eink.lock_bg";
            case "lock_clock": return "persist.sys.a6l.eink.lock_clock";
            case "lock_battery": return "persist.sys.a6l.eink.lock_battery";
            case "reader_sleep": return "persist.sys.a6l.eink.reader_sleep";	// eink-round11, default off
            case "reader_apps": return "persist.sys.a6l.eink.reader_apps";	// eink-round9 (8 Oct): readers | all | custom
            default: throw new IllegalArgumentException("Unknown setting");
        }
    }

    static String defaultValue(String key) {
        switch (key) {
            case "refresh": return "stock";
            case "clear_every": return "10";
            case "fl_enable": case "fl_gamma": case "per_screen": case "lock": case "lock_clock": case "lock_battery": return "1";
            case "lock_bg": return "white";
            case "fl_max": return "100";
            case "ekey": return "sleep";
            case "reader_apps": return "readers";
            case "wallpaper": return "white";
            default: return "0";
        }
    }

    static boolean valid(String key, String value) {
        if (key == null || value == null) return false;
        switch (key) {
            case "theme": return oneOf(value, "light", "dark", "lcd");
            case "refresh": return oneOf(value, "stock", "auto", "quality", "partial", "fast", "fastest");
            case "ekey": return oneOf(value, "sleep", "clear");
            case "reader_apps": return oneOf(value, "readers", "all", "custom");
            case "wallpaper": return oneOf(value, "white", "lcd");
            case "fl_enable": case "mirror_lcd": case "per_screen": case "lock": case "lock_clock": case "lock_battery": case "reader_sleep": return oneOf(value, "0", "1");
            case "lock_bg": return oneOf(value, "white", "black", "image", "lcd");
            case "fl_gamma": return oneOf(value, "1", "2");
            case "clear_every": return inRange(value, 30);
            case "contrast": case "fl_max": return inRange(value, 100);
            default: return false;
        }
    }
    private static boolean oneOf(String value, String... choices) {
        for (String choice : choices) if (choice.equals(value)) return true;
        return false;
    }
    private static boolean inRange(String value, int max) {
        try {
            int n = Integer.parseInt(value);
            return n >= 0 && n <= max && Integer.toString(n).equals(value);
        } catch (NumberFormatException e) { return false; }
    }
}
