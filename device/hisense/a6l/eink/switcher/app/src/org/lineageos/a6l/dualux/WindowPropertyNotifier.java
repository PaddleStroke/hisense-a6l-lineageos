// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** Bounded property-change nudges to WM. Notifications never acknowledge a frame. */
final class WindowPropertyNotifier {
    interface Backend {
        boolean systemUid();
        String prepare();
        String appearance();
        String themeReady();
        void notifyWindow(String request, boolean themeReady);
    }
    private String mTargetAttempted = "", mThemeAttempted = "";

    void published(String request, boolean themeReady, Backend backend) {
        if (!AppearanceGate.valid(request) || !backend.systemUid()) return;
        if (request.equals(themeReady ? mThemeAttempted : mTargetAttempted)) return;
        if (!current(request, themeReady, backend)) return;
        // At most one attempt per phase/pair; errors retain the existing fail-open deadline.
        if (themeReady) mThemeAttempted = request; else mTargetAttempted = request;
        backend.notifyWindow(request, themeReady);
    }

    static boolean current(String request, boolean themeReady, Backend backend) {
        return request.equals(backend.prepare()) && request.equals(backend.appearance())
                && (!themeReady || request.equals(backend.themeReady()))
                && request.equals(backend.prepare());
    }
}
