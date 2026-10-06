// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.app.Application;

public class DualuxApp extends Application {
    @Override public void onCreate() {
        super.onCreate();
        PerScreenMemory.get(this).start();
        LockSync.start(this);	// eink-lockscreen: 12/24 h preference + LCD wallpaper for the e-ink lock screen
    }
}
