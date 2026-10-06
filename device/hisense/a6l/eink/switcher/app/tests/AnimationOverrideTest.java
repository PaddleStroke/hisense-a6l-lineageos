// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
public final class AnimationOverrideTest {
    private static int checks;
    private static void check(boolean ok) { checks++; if (!ok) throw new AssertionError("check " + checks); }
    private static final class Fake implements AnimationOverride.Client, AnimationOverride.Store {
        final Map<String, String> values = new HashMap<>();
        AnimationOverride.Session session; int writes, saves, failWrite = -1; boolean failSave;
        @Override public String get(String key) { return values.get(key); }
        @Override public void set(String key, String value) {
            check(session != null); // durable recovery snapshot precedes every write
            if (writes++ == failWrite) throw new IllegalStateException("provider failure");
            if (value == null) values.remove(key); else values.put(key, value);
        }
        @Override public AnimationOverride.Session load() { return session; }
        @Override public void save(AnimationOverride.Session s) {
            if (failSave) throw new IllegalStateException("storage failure");
            saves++; session = s;
        }
    }
    public static void main(String[] args) {
        for (String[] original : new String[][] {{"0.5", "2.0", "1.500"}, {null, "0", "0.0"}, {"1", null, null}}) {
            Fake f = new Fake();
            for (int i = 0; i < 3; i++) if (original[i] != null) f.values.put(AnimationOverride.KEYS[i], original[i]);
            AnimationOverride a = new AnimationOverride(f, f);
            a.update(false); check(f.session == null && f.writes == 0);
            a.update(true); check(a.suppressed()); check(Arrays.equals(original, f.session.previous));
            int writes = f.writes, saves = f.saves; a.update(true); check(f.writes == writes && f.saves == saves);
            // New controller after process death/reboot recovers persistent settings.
            a = new AnimationOverride(f, f); a.update(false); check(f.session == null);
            for (int i = 0; i < 3; i++) check(java.util.Objects.equals(original[i], f.get(AnimationOverride.KEYS[i])));
        }
        for (int fail = 0; fail < 3; fail++) {
            Fake f = new Fake(); f.failWrite = fail;
            for (String k : AnimationOverride.KEYS) f.values.put(k, "1.25");
            try { new AnimationOverride(f, f).update(true); throw new AssertionError(); } catch (IllegalStateException expected) { check(f.session != null); }
            f.failWrite = -1; new AnimationOverride(f, f).update(false);
            for (String k : AnimationOverride.KEYS) check("1.25".equals(f.get(k)));
        }
        Fake f = new Fake(); f.failSave = true;
        try { new AnimationOverride(f, f).update(true); throw new AssertionError(); } catch (IllegalStateException expected) { check(f.writes == 0); }
        f.failSave = false; AnimationOverride a = new AnimationOverride(f, f); a.update(true);
        f.values.put(AnimationOverride.KEYS[1], "3.75"); a.update(false); check("3.75".equals(f.get(AnimationOverride.KEYS[1])));
        a.update(true); f.values.put(AnimationOverride.KEYS[0], "2.50"); a.update(true); check(a.suppressed());
        a.update(false); check("2.50".equals(f.get(AnimationOverride.KEYS[0])));
        System.out.println("ANIMATION_OVERRIDE_PASS " + checks + " checks");
    }
}
