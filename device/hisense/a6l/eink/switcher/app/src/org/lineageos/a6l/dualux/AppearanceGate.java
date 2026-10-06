// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** Sequence-tagged pre-light barrier. No Android dependencies: host tested. */
final class AppearanceGate {
    interface Backend {
        boolean apply(String request, boolean eink);
        boolean ready(String request, boolean eink);
        void acknowledge(String request);
    }
    private String mRequest = "";
    private boolean mApplied, mAcknowledged;

    void step(String request, long now, Backend backend) {
        if (!valid(request)) { mRequest = ""; mApplied = mAcknowledged = false; return; }
        if (!request.equals(mRequest)) {
            mRequest = request; mApplied = mAcknowledged = false;
        }
        if (mAcknowledged) return;
        boolean eink = request.endsWith(" eink");
        if (!mApplied) {
            if (!backend.apply(request, eink)) return;
            mApplied = true;
        }
        // Configuration, committed redraw and wallpaper ACKs are the barrier;
        // an additional fixed settling sleep adds delay without proving a frame.
        if (!backend.ready(request, eink)) return;
        backend.acknowledge(request); mAcknowledged = true;
    }
    static boolean valid(String request) {
        if (request == null) return false;
        int space = request.indexOf(' ');
        if (space < 1 || space > 19 || (!request.endsWith(" eink") && !request.endsWith(" lcd"))) return false;
        String token = request.substring(0, space);
        for (int i = 0; i < token.length(); i++) if (token.charAt(i) < '0' || token.charAt(i) > '9') return false;
        try { return Long.parseLong(token) > 0 && request.indexOf(' ', space + 1) < 0; }
        catch (NumberFormatException e) { return false; }
    }
}
