// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** Real controller regression: restores user state across switches, failures and app/OS restart. */
public final class ThemeOverrideTest {
    private static int checks;
    private static void check(boolean ok, String reason) {
        checks++;
        if (!ok) throw new AssertionError(reason);
    }
    static final class Fake implements ThemeOverride.Client, ThemeOverride.Store {
        int overlay = ThemeOverride.OFF, writes, saves;
        boolean failSet, failSave;
        ThemeOverride.Session session;
        @Override public int get() { return overlay; }
        @Override public void set(int value) {
            if (failSet) throw new IllegalStateException("Binder failed");
            check(session != null || value == ThemeOverride.OFF,
                    "Recovery state persisted before overlay change");
            overlay = value;
            writes++;
        }
        @Override public ThemeOverride.Session load() { return session; }
        @Override public void save(ThemeOverride.Session value) {
            if (failSave) throw new IllegalStateException("Storage failed");
            session = value;
            saves++;
        }
        ThemeOverride controller() { return new ThemeOverride(this, this); }
    }
    public static void main(String[] args) {
        Fake f = new Fake();
        ThemeOverride c = f.controller();
        c.update(false, "light", 1);
        check(f.writes == 0 && f.saves == 0, "LCD untouched at startup");
        c.update(true, "light", 1);
        check(f.overlay == ThemeOverride.LIGHT, "E-ink light default");
        int writes = f.writes, saves = f.saves;
        for (int i = 0; i < 100; i++) c.update(true, "light", 1);
        check(f.writes == writes && f.saves == saves, "Polling does not repeat Binder or storage writes");
        c.update(true, "dark", 1);
        check(f.overlay == ThemeOverride.DARK && f.session.previous == ThemeOverride.OFF,
                "Changing E-ink preference retains original LCD overlay");
        c = f.controller();
        c.update(false, "dark", 1);
        check(f.overlay == ThemeOverride.OFF && f.session == null, "App restart restores LCD");

        f = new Fake(); f.overlay = ThemeOverride.DARK; c = f.controller();
        c.update(true, "light", 2); c.update(false, "light", 2);
        check(f.overlay == ThemeOverride.DARK, "Existing Attention Mode is restored");
        c.update(true, "light", 2); c.update(true, "lcd", 2);
        check(f.overlay == ThemeOverride.DARK && f.session == null, "Follow LCD exits override while on rear");

        f = new Fake(); c = f.controller(); c.update(true, "light", 3);
        f.overlay = ThemeOverride.DARK;
        c.update(false, "light", 3);
        check(f.overlay == ThemeOverride.DARK && f.session == null, "LCD return preserves newer external change");
        c.update(true, "light", 3);
        f.overlay = ThemeOverride.OFF;
        c.update(true, "light", 3); c.update(false, "light", 3);
        check(f.overlay == ThemeOverride.OFF, "Active external change becomes new restoration baseline");

        f = new Fake(); f.overlay = ThemeOverride.DARK; c = f.controller();
        c.update(true, "light", 4);
        f.overlay = ThemeOverride.OFF; // system-server overlay resets across OS reboot
        c = f.controller(); c.update(false, "light", 5);
        check(f.overlay == ThemeOverride.OFF && f.session == null, "Reboot does not restore stale overlay");
        c.update(true, "dark", 5); c.update(false, "dark", 5);
        check(f.overlay == ThemeOverride.OFF, "New boot gets its own baseline");

        f = new Fake(); c = f.controller(); f.failSet = true;
        try { c.update(true, "light", 6); throw new AssertionError("missing Binder failure"); }
        catch (IllegalStateException expected) { }
        check(f.session != null && f.overlay == ThemeOverride.OFF, "Failed Binder call retains recovery state");
        f.failSet = false; c = f.controller(); c.update(false, "light", 6);
        check(f.overlay == ThemeOverride.OFF && f.session == null, "Failed apply leaves LCD unchanged");
        f.failSave = true;
        try { c.update(true, "light", 6); throw new AssertionError("missing storage failure"); }
        catch (IllegalStateException expected) { }
        check(f.overlay == ThemeOverride.OFF && f.session == null, "No overlay change without durable recovery state");

        f = new Fake(); c = f.controller(); f.overlay = -1;
        c.update(true, "light", 7);
        check(f.writes == 0 && f.saves == 0, "Unsupported API state leaves preferences unchanged");
        f.overlay = ThemeOverride.OFF; c.update(true, "light", 7);
        check(f.overlay == ThemeOverride.LIGHT, "Unavailable provider is retried later");
        System.out.println("THEME_OVERRIDE_PASS " + checks + " checks");
    }
}
