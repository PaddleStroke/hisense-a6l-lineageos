// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings: full e-ink clear (anti-ghosting flash) + redraw. */
public class ClearTile extends TileService {
    @Override public void onStartListening() {
        Tile t = getQsTile();
        if (t == null) return;
        t.setState(Dualux.daemonRunning() ? Tile.STATE_INACTIVE : Tile.STATE_UNAVAILABLE);
        t.setLabel(getString(R.string.tile_clear));
        t.updateTile();
    }

    @Override public void onClick() { Dualux.request("clear"); }
}
