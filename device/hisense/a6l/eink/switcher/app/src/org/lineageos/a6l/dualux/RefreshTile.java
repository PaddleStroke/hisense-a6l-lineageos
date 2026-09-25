// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings: e-ink refresh mode; tap cycles auto -> quality -> reading -> fast -> fastest (applied live). */
public class RefreshTile extends TileService {
    @Override public void onStartListening() { update(); }

    @Override public void onClick() {
        Dualux.set(Dualux.P_REFRESH, Dualux.nextMode(Dualux.refreshMode()));
        update();
    }

    private void update() {
        Tile t = getQsTile();
        if (t == null) return;
        t.setState(Tile.STATE_ACTIVE);
        t.setLabel(getString(R.string.tile_refresh));
        t.setSubtitle(Dualux.shortName(Dualux.refreshMode()));
        t.updateTile();
    }
}
