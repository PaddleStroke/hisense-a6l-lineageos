// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** Temporary day/night overlay. The user's night-mode setting/schedule is never changed. */
final class ThemeOverride {
    static final int OFF = 1000, DARK = 1001, LIGHT = 1002;

    interface Client {
        int get();
        void set(int value);
    }

    interface Store {
        Session load();
        void save(Session session);
    }

    static final class Session {
        final int previous, applied, boot;
        Session(int previous, int applied, int boot) {
            this.previous = previous;
            this.applied = applied;
            this.boot = boot;
        }
    }

    private final Client mClient;
    private final Store mStore;

    ThemeOverride(Client client, Store store) { mClient = client; mStore = store; }

    void update(boolean eink, String choice, int boot) {
        Session s = mStore.load();
        // The framework overlay is in memory only; stale persisted sessions from
        // an earlier boot must not resurrect an old Attention Mode on the LCD.
        if (s != null && s.boot != boot) {
            mStore.save(null);
            s = null;
        }
        int current = mClient.get();
        if (current != OFF && current != DARK && current != LIGHT) return;
        int desired = !eink || "lcd".equals(choice) ? OFF
                : "dark".equals(choice) ? DARK : LIGHT;
        if (desired == OFF) {
            if (s == null) return;
            // Leave a newer external overlay alone; we own only the value we set.
            if (current == s.applied && current != s.previous) mClient.set(s.previous);
            mStore.save(null);
            return;
        }
        int previous = s == null ? current
                : current != s.applied ? current : s.previous;
        if (s == null || s.previous != previous || s.applied != desired) {
            // Persist recovery state before the Binder call: survive app death.
            mStore.save(new Session(previous, desired, boot));
        }
        if (current != desired) mClient.set(desired);
    }
}
