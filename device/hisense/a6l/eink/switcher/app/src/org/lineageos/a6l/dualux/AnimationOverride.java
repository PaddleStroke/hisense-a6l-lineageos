// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import java.util.Arrays;
import java.util.Objects;

/** Persist exact prior scales before suppressing animations, including unset keys.
 * Scales survive reboot, so recovery sessions must survive reboot too. */
final class AnimationOverride {
    static final String[] KEYS = {"window_animation_scale", "transition_animation_scale",
            "animator_duration_scale"};
    static final String OFF = "0";
    interface Client { String get(String key); void set(String key, String value); }
    interface Store { Session load(); void save(Session session); }
    static final class Session {
        final String[] previous;
        Session(String[] previous) { this.previous = previous.clone(); }
    }
    private final Client mClient;
    private final Store mStore;
    AnimationOverride(Client client, Store store) { mClient = client; mStore = store; }
    void update(boolean eink) {
        Session session = mStore.load();
        if (!eink) {
            if (session == null) return;
            for (int i = 0; i < KEYS.length; i++) {
                // Do not overwrite a newer explicit external setting.
                if (OFF.equals(mClient.get(KEYS[i]))) mClient.set(KEYS[i], session.previous[i]);
            }
            mStore.save(null); return;
        }
        String[] previous = session == null ? new String[KEYS.length] : session.previous.clone();
        for (int i = 0; i < KEYS.length; i++) {
            String current = mClient.get(KEYS[i]);
            if (session == null || !OFF.equals(current)) previous[i] = current;
        }
        if (session == null || !Arrays.equals(previous, session.previous)) {
            // Atomic durable snapshot before the first provider write; recover
            // after process death between any of the three writes.
            mStore.save(new Session(previous));
        }
        for (String key : KEYS) if (!Objects.equals(mClient.get(key), OFF)) mClient.set(key, OFF);
    }
    boolean suppressed() {
        for (String key : KEYS) if (!OFF.equals(mClient.get(key))) return false;
        return true;
    }
}
