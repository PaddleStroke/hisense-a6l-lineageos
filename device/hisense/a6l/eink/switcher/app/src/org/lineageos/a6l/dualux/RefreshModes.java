// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** eink-round4: the refresh modes, in the order of the Settings E-ink tab list; the QS tile cycles through all of
 * them, "stock" included. No Android dependencies: host tested (RefreshModesTest). */
final class RefreshModes {
    static final String[] MODES = {"stock", "auto", "quality", "partial", "fast", "fastest"};
    private RefreshModes() { }

    /** Known value, else "stock" (the mirror's default when the property is unset). */
    static String normalize(String m) {
        for (String k : MODES) if (k.equals(m)) return m;
        return MODES[0];
    }

    static String next(String m) {
        for (int i = 0; i < MODES.length; i++) if (MODES[i].equals(m)) return MODES[(i + 1) % MODES.length];
        return MODES[0];
    }
}
