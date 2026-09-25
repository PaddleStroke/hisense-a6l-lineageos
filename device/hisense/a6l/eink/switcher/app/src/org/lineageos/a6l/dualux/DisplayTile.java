// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.os.Handler;
import android.os.Looper;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings: which screen is the phone's screen (E-ink / LCD). Tap = switch. */
public class DisplayTile extends TileService {
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Runnable mUpdate = this::update;

    @Override public void onStartListening() { update(); }
    @Override public void onStopListening() { mHandler.removeCallbacks(mUpdate); }

    @Override public void onClick() {
        Dualux.request(Dualux.isEink() ? "lcd" : "eink");
        mHandler.postDelayed(mUpdate, 400);
        mHandler.postDelayed(mUpdate, 1200);
    }

    private void update() {
        Tile t = getQsTile();
        if (t == null) return;
        boolean running = Dualux.daemonRunning(), eink = Dualux.isEink();
        t.setState(!running ? Tile.STATE_UNAVAILABLE : eink ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        t.setLabel(getString(R.string.tile_display));
        t.setSubtitle(getString(eink ? R.string.screen_eink : R.string.screen_lcd));
        t.updateTile();
    }
}
