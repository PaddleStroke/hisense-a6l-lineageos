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

    /** @return the value written to contrast_level, or null when nothing was written (eink-round6f: logged by the caller) */
    String update(boolean wanted) {
        Session session = mStore.load();
        String current = mClient.get();
        if (!wanted) {
            if (session == null) return null;
            // Do not overwrite a newer explicit user choice made while the e-ink theme was active.
            String written = null;
            if (HIGH.equals(current)) { mClient.set(session.previous); written = session.previous == null ? "(unset)" : session.previous; }
            mStore.save(null);
            return written;
        }
        if (HIGH.equals(current) && session != null) return null;
        String previous = session == null || !HIGH.equals(current) ? current : session.previous;
        if (session == null || !Objects.equals(previous, session.previous)) {
            mStore.save(new Session(previous));	// durable snapshot before the provider write
        }
        if (!HIGH.equals(current)) { mClient.set(HIGH); return HIGH; }
        return null;
    }

    boolean active() { return HIGH.equals(mClient.get()); }
}
