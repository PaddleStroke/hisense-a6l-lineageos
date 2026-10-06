// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import java.util.Objects;

/** eink-round2: Material "high contrast" colour scheme (Settings.Secure.CONTRAST_LEVEL = 1.0) while the e-ink shows an
 * e-ink theme. SystemUI's ThemeOverlayController regenerates the dynamic colours with that contrast: near-black text
 * and near-white surfaces instead of the tinted grey cards and grey secondary text filmed on 6 Oct. The exact prior
 * value (including "unset") is persisted before the first write and restored on the LCD, like AnimationOverride. */
final class ContrastOverride {
    static final String KEY = "contrast_level";
    static final String HIGH = "1.0";
    interface Client { String get(); void set(String value); }
    interface Store { Session load(); void save(Session session); }
    static final class Session {
        final String previous;
        Session(String previous) { this.previous = previous; }
    }
    private final Client mClient;
    private final Store mStore;
    ContrastOverride(Client client, Store store) { mClient = client; mStore = store; }

    void update(boolean wanted) {
        Session session = mStore.load();
        String current = mClient.get();
        if (!wanted) {
            if (session == null) return;
            // Do not overwrite a newer explicit user choice made while the e-ink theme was active.
            if (HIGH.equals(current)) mClient.set(session.previous);
            mStore.save(null);
            return;
        }
        if (HIGH.equals(current) && session != null) return;
        String previous = session == null || !HIGH.equals(current) ? current : session.previous;
        if (session == null || !Objects.equals(previous, session.previous)) {
            mStore.save(new Session(previous));	// durable snapshot before the provider write
        }
        if (!HIGH.equals(current)) mClient.set(HIGH);
    }

    boolean active() { return HIGH.equals(mClient.get()); }
}
