// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** eink-round4: the QS tile cycle includes "stock" and visits exactly the values the Settings provider accepts. */
public final class RefreshModesTest {
    private static int checks;
    private static void check(boolean result, String name) {
        if (!result) throw new AssertionError(name);
        checks++;
    }
    public static void main(String[] args) {
        check("stock".equals(RefreshModes.MODES[0]), "stock is the first mode (Settings list order)");
        String m = "stock"; java.util.Set<String> seen = new java.util.LinkedHashSet<>();
        for (int i = 0; i < RefreshModes.MODES.length; i++) { seen.add(m); m = RefreshModes.next(m); }
        check(seen.size() == RefreshModes.MODES.length && "stock".equals(m), "tile cycle visits every mode once and returns to stock: " + seen);
        check("stock".equals(RefreshModes.next("fastest")), "fastest -> stock");
        check("auto".equals(RefreshModes.next("stock")), "stock -> auto");
        check("stock".equals(RefreshModes.next("bogus")) && "stock".equals(RefreshModes.normalize("")), "unknown/unset -> stock");
        for (String mode : RefreshModes.MODES) check(SettingsValues.valid("refresh", mode), "provider accepts " + mode);
        check("stock".equals(SettingsValues.defaultValue("refresh")), "provider default is stock");
        System.out.println("REFRESH_MODES_PASS " + checks);
    }
}
