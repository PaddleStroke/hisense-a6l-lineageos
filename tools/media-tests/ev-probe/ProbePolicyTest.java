package org.a6l.evprobe;

public final class ProbePolicyTest {
    private static int checks;
    private static void check(boolean value) { ++checks; if (!value) throw new AssertionError(checks); }
    private static void rejected(int low, int high, int num, int den) {
        try { ProbePolicy.minusOneStep(low, high, num, den); throw new AssertionError("accepted unsupported EV"); }
        catch (IllegalArgumentException expected) { ++checks; }
    }
    public static void main(String[] args) {
        check(ProbePolicy.minusOneStep(-3, 3, 1, 3) == -3);
        check(ProbePolicy.minusOneStep(-6, 6, 1, 6) == -6);
        check(ProbePolicy.minusOneStep(-2, 2, 1, 2) == -2);
        rejected(0, 0, 1, 3); rejected(-2, 2, 1, 3); rejected(-3, 3, 2, 1);
        rejected(-3, 3, 0, 1); rejected(-3, 3, 1, 0); rejected(1, 3, 1, 3);
        ProbePolicy p = new ProbePolicy();
        check(!p.shouldCapture(0, 0, 0, true));
        check(!p.shouldCapture(100, 0, 0, true));
        check(!p.shouldCapture(2499, 0, 0, true));
        check(p.shouldCapture(2500, 0, 0, true));
        check(!p.shouldCapture(2600, -3, 0, true));
        check(!p.shouldCapture(2700, -3, -3, true));
        check(!p.shouldCapture(2800, -3, -3, true));
        check(p.shouldCapture(2900, -3, -3, true));
        check(!p.shouldCapture(3000, -3, null, true));
        check(!p.shouldCapture(3100, -3, -3, false));
        check(!p.shouldCapture(11999, -3, -3, false));
        check(p.shouldCapture(12000, -3, -3, false));
        p = new ProbePolicy();
        check(!p.shouldCapture(3000, 0, 0, true));
        ProbePolicy.validatePair(100L, 100L, -3, -3, 1600, 1200, 1600, 1200); ++checks;
        Object[][] bad = {{null, 100L, -3, 1600, 1200}, {101L, 100L, -3, 1600, 1200},
                {100L, 100L, null, 1600, 1200}, {100L, 100L, 0, 1600, 1200},
                {100L, 100L, -3, 1200, 1600}, {100L, 100L, -3, -1, -1}};
        for (Object[] values : bad) {
            try {
                ProbePolicy.validatePair((Long)values[0], (Long)values[1], (Integer)values[2], -3,
                        (Integer)values[3], (Integer)values[4], 1600, 1200);
                throw new AssertionError("accepted unmatched JPEG");
            } catch (IllegalArgumentException expected) { ++checks; }
        }
        System.out.println("PASS " + checks + " actual EV/settling policy checks");
    }
}
