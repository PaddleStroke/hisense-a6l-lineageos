package org.a6l.evprobe;

/** Pure policy shared by the actual Camera2 Activity and host regression. */
final class ProbePolicy {
    static final long MIN_SETTLE_MS = 2500;
    static final long MAX_SETTLE_MS = 12000;
    private int consecutive;

    static int minusOneStep(int lower, int upper, int numerator, int denominator) {
        if (lower > 0 || upper < 0 || numerator <= 0 || denominator <= 0)
            throw new IllegalArgumentException("0 EV or positive step unavailable");
        double step = (double) numerator / denominator;
        int value = (int) Math.round(-1.0 / step);
        if (value >= 0 || value < lower || value > upper || Math.abs(value * step + 1) > 0.05)
            throw new IllegalArgumentException("-1 EV is not representable by this camera");
        return value;
    }

    static void validatePair(Long resultTimestamp, long imageTimestamp, Integer reportedStep,
                             int expectedStep, int width, int height, int expectedWidth, int expectedHeight) {
        if (resultTimestamp == null || resultTimestamp != imageTimestamp)
            throw new IllegalArgumentException("JPEG/result sensor timestamp mismatch");
        if (reportedStep == null || reportedStep != expectedStep)
            throw new IllegalArgumentException("Still compensation missing/mismatched");
        if (width != expectedWidth || height != expectedHeight)
            throw new IllegalArgumentException("Decoded JPEG dimensions differ from negotiated output");
    }

    /** A convergence indication is evidence, not proof of physical exposure. */
    boolean shouldCapture(long elapsedMs, int expected, Integer reported, boolean converged) {
        consecutive = reported != null && reported == expected && converged ? consecutive + 1 : 0;
        return elapsedMs >= MAX_SETTLE_MS || elapsedMs >= MIN_SETTLE_MS && consecutive >= 3;
    }
}
