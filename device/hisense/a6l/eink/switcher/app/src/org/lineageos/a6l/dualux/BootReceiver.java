// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** The app is persistent (started by the system at boot); this only makes sure the per-screen memory runs. */
public class BootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        PerScreenMemory.get(context.getApplicationContext()).start();
    }
}
