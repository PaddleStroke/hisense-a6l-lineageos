// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings: e-ink frontlight on/off (its level follows the normal brightness slider while the e-ink is active). */
public class FrontlightTile extends TileService {
    @Override public void onStartListening() { update(); }

    @Override public void onClick() {
        boolean on = Dualux.getInt(Dualux.P_FL_ENABLE, 1) != 0;
        Dualux.set(Dualux.P_FL_ENABLE, on ? "0" : "1");
        update();
    }

    private void update() {
        Tile t = getQsTile();
        if (t == null) return;
        boolean on = Dualux.getInt(Dualux.P_FL_ENABLE, 1) != 0;
        t.setState(on ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        t.setLabel(getString(R.string.tile_frontlight));
        t.updateTile();
    }
}
