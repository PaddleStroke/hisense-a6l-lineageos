// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** Boundary checks for values crossing the privileged Settings provider. */
public final class SettingsValuesTest {
    private static int checks;
    private static void check(boolean result, String name) {
        if (!result) throw new AssertionError(name);
        checks++;
    }
    public static void main(String[] args) {
        for (String key : SettingsValues.KEYS) {
            check(SettingsValues.valid(key, SettingsValues.defaultValue(key)), "default " + key);
            check(SettingsValues.property(key).startsWith("persist.sys.a6l."), "owned property " + key);
            check(!SettingsValues.valid(key, ""), "empty " + key);
            check(!SettingsValues.valid(key, "1\n"), "newline " + key);
            check(!SettingsValues.valid(key, null), "null " + key);
        }
        check(!SettingsValues.valid("persist.sys.usb.config", "adb"), "arbitrary key");
        check(!SettingsValues.valid(null, "1"), "null key");
        check(!SettingsValues.valid("theme", "auto"), "unknown theme");
        check(SettingsValues.valid("theme", "light"), "light");
        check(SettingsValues.valid("theme", "dark"), "dark");
        check(SettingsValues.valid("theme", "lcd"), "follow LCD");
        for (String key : new String[]{"clear_every", "contrast", "fl_max"}) {
            int max = "clear_every".equals(key) ? 30 : 100;
            check(SettingsValues.valid(key, "0"), "zero " + key);
            check(SettingsValues.valid(key, Integer.toString(max)), "upper " + key);
            for (String bad : new String[]{"-1", Integer.toString(max + 1), "01", "+1", " 1", "2147483648"}) {
                check(!SettingsValues.valid(key, bad), "reject " + key + " " + bad);
            }
        }
        try {
            SettingsValues.property("sys.powerctl");
            throw new AssertionError("arbitrary property lookup");
        } catch (IllegalArgumentException expected) { checks++; }
        System.out.println("SETTINGS_VALUES_PASS " + checks);
    }
}
