// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** WindowManager owns the animation scales; a headless process's animator cache can be stale. */
final class AnimationReadiness {
    private AnimationReadiness() { }

    static boolean authoritativeSuppressed() throws ReflectiveOperationException {
        Object service = Class.forName("android.view.WindowManagerGlobal")
                .getMethod("getWindowManagerService").invoke(null);
        float[] scales = (float[]) Class.forName("android.view.IWindowManager")
                .getMethod("getAnimationScales").invoke(service);
        return suppressed(scales);
    }

    static boolean suppressed(float[] scales) {
        return scales != null && scales.length == 3
                && scales[0] == 0f && scales[1] == 0f && scales[2] == 0f;
    }
}
