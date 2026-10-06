// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** eink-round4: where the e-ink settings live (Settings > Display > E-ink tab). No Android dependencies: host tested. */
final class SettingsTarget {
    static final String ACTION = "android.settings.DISPLAY_SETTINGS";	// Settings.ACTION_DISPLAY_SETTINGS
    static final String PACKAGE = "com.android.settings";
    static final String EXTRA_FRAGMENT_ARG_KEY = ":settings:fragment_args_key";	// SettingsActivity.EXTRA_FRAGMENT_ARG_KEY
    /** A preference of the E-ink tab (rom patch packages/apps/Settings/0001-a6l-display-tabs.patch, A6lEinkPreferences). */
    static final String EINK_KEY = "a6l_eink_switch";
    private SettingsTarget() { }
}
