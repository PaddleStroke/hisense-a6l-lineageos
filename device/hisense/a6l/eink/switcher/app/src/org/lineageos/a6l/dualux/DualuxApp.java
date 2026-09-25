// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.app.Application;

public class DualuxApp extends Application {
    @Override public void onCreate() {
        super.onCreate();
        PerScreenMemory.get(this).start();
    }
}
