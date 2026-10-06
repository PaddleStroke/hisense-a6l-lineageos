// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/**
 * eink-round4: the Settings app (Display, E-ink tab) is the only e-ink settings UI. This invisible activity keeps the
 * entry points that used to open the removed "E-ink display" screen: Quick Settings tile long-press
 * (QS_TILE_PREFERENCES) and org.lineageos.a6l.dualux.SETTINGS. It forwards to Settings > Display with the E-ink tab
 * selected (DisplaySettings picks the tab that holds the preference named by :settings:fragment_args_key).
 */
public class SettingsLink extends Activity {
    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        Intent target = new Intent(SettingsTarget.ACTION)
                .setPackage(SettingsTarget.PACKAGE)
                .putExtra(SettingsTarget.EXTRA_FRAGMENT_ARG_KEY, SettingsTarget.EINK_KEY)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        try {
            startActivity(target);
        } catch (ActivityNotFoundException e) {
            Log.w(Dualux.TAG, "Settings > Display not available", e);
        }
        finish();
    }
}
