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

    /** eink-round4: the Material contrast level may only change while no screen switch is in flight (prepare empty):
     * its overlay regeneration kept WM's sync engine busy past the themed-redraw deadline (3 s switches). */
    static boolean contrastMayChange(String prepare) { return prepare == null || prepare.isEmpty(); }

    /** eink-round6f: and only once the screen has been settled (no prepare, same screen) for CONTRAST_SETTLE_MS. The
     * overlay regeneration a contrast change starts takes ~1-2 s (FRRO + idmap + relaunch of every activity); landing in
     * a switch it makes WM miss the themed-redraw deadline (3 s fail-open, 7 Oct 17:25:49 / 17:26:25). Waiting also
     * means a quick e-ink visit (back to the LCD within the settle) never toggles the contrast at all. */
    static final long CONTRAST_SETTLE_MS = 2000;
    static boolean contrastMayChange(String prepare, long lastSwitchActivityMs, long nowMs) {
        return contrastMayChange(prepare) && nowMs - lastSwitchActivityMs >= CONTRAST_SETTLE_MS;
    }

    /** eink-round2: the daemon can fail open (clear its prepare) before this app ever applied the target, e.g. a
     * prepare begun while Android slept. Bookkeeping then restores theme and animations, but the WM-facing target
     * (sys.a6l.dualux.appearance) keeps the old screen: the rear white wallpaper stayed over the LCD home (user
     * report 6 Oct). Returns the target to publish, or null when it already matches or a switch is in flight. */
    static String reconcile(String appearance, String prepare, String screen, long token) {
        if (prepare != null && !prepare.isEmpty()) return null;	// the gate owns an in-flight switch
        if (!"eink".equals(screen) && !"lcd".equals(screen)) return null;
        if (token <= 0) return null;
        boolean valid = valid(appearance);
        if (valid && appearance.endsWith(" " + screen)) return null;
        if (!valid && "lcd".equals(screen)) return null;	// never prepared since boot: nothing was overridden
        return token + " " + screen;
    }
}
