// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;
public final class AppearanceGateTest {
    private static int checks;
    private static void check(boolean ok) { checks++; if (!ok) throw new AssertionError("check " + checks); }
    private static final class Fake implements AppearanceGate.Backend {
        int applies, acks; boolean applyOk = true, ready; String applied, ack;
        @Override public boolean apply(String request, boolean eink) { applies++; applied = request; check(eink == request.endsWith(" eink")); return applyOk; }
        @Override public boolean ready(String request, boolean eink) { return ready && request.equals(applied); }
        @Override public void acknowledge(String request) { acks++; ack = request; }
    }
    public static void main(String[] args) {
        AppearanceGate gate = new AppearanceGate(); Fake f = new Fake();
        for (String s : new String[] {"", "0 eink", "-1 lcd", "1 unknown", "1 eink extra", "x eink", "9223372036854775808 lcd", "1  eink"}) check(!AppearanceGate.valid(s));
        check(AppearanceGate.valid("1 eink") && AppearanceGate.valid("9223372036854775807 lcd"));
        gate.step("1 eink", 0, f); check(f.applies == 1 && f.acks == 0);
        f.ready = true; gate.step("1 eink", 199, f); check(f.acks == 1);
        gate.step("1 eink", 200, f); check(f.acks == 1 && "1 eink".equals(f.ack));
        gate.step("1 eink", 1000, f); check(f.applies == 1 && f.acks == 1);
        f.ready = false; gate.step("2 lcd", 1000, f); gate.step("2 lcd", 1200, f); check(f.applies == 2 && f.acks == 1);
        // A superseding request must be freshly applied and ready, never acknowledge the old request.
        gate.step("3 eink", 1300, f); gate.step("3 eink", 1499, f); check(f.acks == 1);
        f.ready = true; gate.step("3 eink", 1500, f); check(f.acks == 2 && "3 eink".equals(f.ack));
        gate.step("", 1600, f); f.applyOk = false; gate.step("4 lcd", 2000, f); gate.step("4 lcd", 3000, f); check(f.acks == 2);
        f.applyOk = true; gate.step("4 lcd", 3100, f); gate.step("4 lcd", 3300, f); check(f.acks == 3 && "4 lcd".equals(f.ack));
        // eink-round2: target reconciliation after a daemon fail-open
        check("77 lcd".equals(AppearanceGate.reconcile("5 eink", "", "lcd", 77)));	// stale rear target over the LCD
        check("77 lcd".equals(AppearanceGate.reconcile("5 eink", null, "lcd", 77)));
        check("77 eink".equals(AppearanceGate.reconcile("5 lcd", "", "eink", 77)));
        check("77 eink".equals(AppearanceGate.reconcile("", "", "eink", 77)));
        check("77 eink".equals(AppearanceGate.reconcile("garbage", "", "eink", 77)));
        check(AppearanceGate.reconcile("", "", "lcd", 77) == null);		// boot: never prepared
        check(AppearanceGate.reconcile("5 lcd", "", "lcd", 77) == null && AppearanceGate.reconcile("5 eink", "", "eink", 77) == null);
        check(AppearanceGate.reconcile("5 eink", "9 lcd", "lcd", 77) == null);	// in-flight switch belongs to the gate
        check(AppearanceGate.reconcile("5 eink", "", "", 77) == null && AppearanceGate.reconcile("5 eink", "", "lcd", 0) == null);
        // eink-round4: the contrast level never changes while a switch is in flight (3 s switches, 6 Oct 17:01)
        check(!AppearanceGate.contrastMayChange("1510431138 eink") && !AppearanceGate.contrastMayChange("9 lcd"));
        check(AppearanceGate.contrastMayChange("") && AppearanceGate.contrastMayChange(null));
        // eink-round6f: and only CONTRAST_SETTLE_MS after the last switch activity (prepare / screen change)
        check(!AppearanceGate.contrastMayChange("", 10_000, 10_000) && !AppearanceGate.contrastMayChange("", 10_000, 11_999));
        check(AppearanceGate.contrastMayChange("", 10_000, 12_000) && AppearanceGate.contrastMayChange(null, 10_000, 20_000));
        check(!AppearanceGate.contrastMayChange("5 lcd", 0, 1_000_000));
        System.out.println("APPEARANCE_GATE_TEST PASS " + checks);
    }
}
