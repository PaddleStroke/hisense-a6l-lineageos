// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;
import java.util.Objects;
public final class ContrastOverrideTest {
    private static int checks;
    private static void check(boolean ok) { checks++; if (!ok) throw new AssertionError("check " + checks); }
    private static final class Fake implements ContrastOverride.Client, ContrastOverride.Store {
        String value; ContrastOverride.Session session; int writes, saves; boolean failWrite;
        @Override public String get() { return value; }
        @Override public void set(String v) {
            check(session != null);	// the recovery snapshot precedes every write
            writes++; if (failWrite) throw new IllegalStateException("provider failure");
            value = v;
        }
        @Override public ContrastOverride.Session load() { return session; }
        @Override public void save(ContrastOverride.Session s) { saves++; session = s; }
    }
    public static void main(String[] args) {
        for (String original : new String[] {null, "0.0", "0.5", "-1.0", "1.0"}) {
            Fake f = new Fake(); f.value = original;
            ContrastOverride c = new ContrastOverride(f, f);
            c.update(false); check(f.session == null && f.writes == 0);
            c.update(true); check(c.active() && f.session != null && Objects.equals(f.session.previous, original));
            int w = f.writes, s = f.saves; c.update(true); check(f.writes == w && f.saves == s);	// idempotent
            c = new ContrastOverride(f, f); c.update(false);	// new process after death/reboot restores
            check(f.session == null && Objects.equals(f.value, original));
        }
        // A user choice made while the e-ink theme was active is kept on the LCD
        Fake f = new Fake(); f.value = "0.0"; ContrastOverride c = new ContrastOverride(f, f);
        c.update(true); f.value = "0.5"; c.update(false); check("0.5".equals(f.value) && f.session == null);
        // ... and re-applying while active records that newer choice as the value to restore
        f.value = "0.0"; c.update(true); f.value = "-0.5"; c.update(true); check(c.active() && "-0.5".equals(f.session.previous));
        c.update(false); check("-0.5".equals(f.value));
        // provider failure after the snapshot: recoverable on the next update(false)
        f = new Fake(); f.value = "0.0"; f.failWrite = true;
        try { new ContrastOverride(f, f).update(true); throw new AssertionError(); } catch (IllegalStateException expected) { check(f.session != null); }
        f.failWrite = false; new ContrastOverride(f, f).update(false); check(f.session == null && "0.0".equals(f.value));
        System.out.println("CONTRAST_OVERRIDE_PASS " + checks + " checks");
    }
}
