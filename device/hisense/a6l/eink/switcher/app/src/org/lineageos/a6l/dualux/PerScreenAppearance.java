// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.app.UiModeManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.provider.Settings;
import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/** E-ink appearance preferences; wallpaper preferences are not yet implemented. */
final class PerScreenAppearance {
    private static PerScreenAppearance sInstance;
    private final Context mContext;
    private final SharedPreferences mPrefs;
    private ThemeOverride mTheme;
    private boolean mReportedFailure;
    private Object mUiManager;
    private Method mOverlayGetter;
    private final AnimationOverride mAnimations;
    private final ContrastOverride mContrast;
    private String mReadyReason = "not_checked";

    static synchronized PerScreenAppearance get(Context context) {
        if (sInstance == null) sInstance = new PerScreenAppearance(context);
        return sInstance;
    }

    private PerScreenAppearance(Context context) {
        mContext = context.getApplicationContext().createDeviceProtectedStorageContext();
        mPrefs = mContext.getSharedPreferences("appearance", Context.MODE_PRIVATE);
        mAnimations = new AnimationOverride(new AnimationOverride.Client() {
            @Override public String get(String key) {
                return Settings.Global.getString(mContext.getContentResolver(), key);
            }
            @Override public void set(String key, String value) {
                if (!Settings.Global.putString(mContext.getContentResolver(), key, value)) {
                    throw new IllegalStateException("Cannot update animation scale " + key);
                }
            }
        }, new AnimationOverride.Store() {
            @Override public AnimationOverride.Session load() {
                if (!mPrefs.getBoolean("animations_active", false)) return null;
                String[] previous = new String[AnimationOverride.KEYS.length];
                for (int i = 0; i < previous.length; i++) previous[i] = mPrefs.getString("animation_previous_" + i, null);
                return new AnimationOverride.Session(previous);
            }
            @Override public void save(AnimationOverride.Session session) {
                SharedPreferences.Editor editor = mPrefs.edit();
                editor.putBoolean("animations_active", session != null);
                for (int i = 0; i < AnimationOverride.KEYS.length; i++) {
                    if (session == null || session.previous[i] == null) editor.remove("animation_previous_" + i);
                    else editor.putString("animation_previous_" + i, session.previous[i]);
                }
                if (!editor.commit()) throw new IllegalStateException("Cannot persist animation recovery state");
            }
        });
        mContrast = new ContrastOverride(new ContrastOverride.Client() {
            @Override public String get() {
                return Settings.Secure.getString(mContext.getContentResolver(), ContrastOverride.KEY);
            }
            @Override public void set(String value) {
                if (!Settings.Secure.putString(mContext.getContentResolver(), ContrastOverride.KEY, value)) {
                    throw new IllegalStateException("Cannot update " + ContrastOverride.KEY);
                }
            }
        }, new ContrastOverride.Store() {
            @Override public ContrastOverride.Session load() {
                if (!mPrefs.getBoolean("contrast_active", false)) return null;
                return new ContrastOverride.Session(mPrefs.getString("contrast_previous", null));
            }
            @Override public void save(ContrastOverride.Session s) {
                SharedPreferences.Editor editor = mPrefs.edit();
                if (s == null) editor.remove("contrast_active").remove("contrast_previous");
                else if (s.previous == null) editor.putBoolean("contrast_active", true).remove("contrast_previous");
                else editor.putBoolean("contrast_active", true).putString("contrast_previous", s.previous);
                if (!editor.commit()) throw new IllegalStateException("Cannot persist contrast recovery state");
            }
        });
        try {
            UiModeManager manager = mContext.getSystemService(UiModeManager.class);
            Method getter = UiModeManager.class.getMethod("getAttentionModeThemeOverlay");
            mUiManager = manager; mOverlayGetter = getter;
            Method setter = UiModeManager.class.getMethod("setAttentionModeThemeOverlay", int.class);
            mTheme = new ThemeOverride(new ThemeOverride.Client() {
                @Override public int get() { return (Integer) invoke(getter, manager); }
                @Override public void set(int value) { invoke(setter, manager, value); }
            }, new ThemeOverride.Store() {
                @Override public ThemeOverride.Session load() {
                    if (!mPrefs.getBoolean("theme_active", false)) return null;
                    return new ThemeOverride.Session(mPrefs.getInt("theme_previous", ThemeOverride.OFF),
                            mPrefs.getInt("theme_applied", ThemeOverride.LIGHT),
                            mPrefs.getInt("theme_boot", -1));
                }
                @Override public void save(ThemeOverride.Session s) {
                    SharedPreferences.Editor editor = mPrefs.edit();
                    if (s == null) editor.remove("theme_active").remove("theme_previous")
                            .remove("theme_applied").remove("theme_boot");
                    else editor.putBoolean("theme_active", true).putInt("theme_previous", s.previous)
                            .putInt("theme_applied", s.applied).putInt("theme_boot", s.boot);
                    if (!editor.commit()) throw new IllegalStateException("Cannot persist theme recovery state");
                }
            });
        } catch (ReflectiveOperationException | RuntimeException e) {
            report(e);
        }
    }

    String theme() { return mPrefs.getString("eink_theme", "light"); }

    void setTheme(String choice) {
        if (!"light".equals(choice) && !"dark".equals(choice) && !"lcd".equals(choice)) return;
        mPrefs.edit().putString("eink_theme", choice).apply();
        update(Dualux.daemonRunning() && Dualux.isEink(), !AppearanceGate.contrastMayChange(Dualux.get(Dualux.PREPARE, "")));
    }

    boolean available() { return mTheme != null; }

    /** @param switching a screen switch is in flight (vendor.dualux.prepare set): leave the contrast level alone. */
    boolean update(boolean eink, boolean switching) {
        try {
            mAnimations.update(eink);
            // eink-round2: high-contrast Material scheme with the e-ink themes; "Follow the LCD theme" keeps the LCD look.
            // eink-round4: never during a switch. The contrast change regenerates the theme overlays (assets-paths
            // config change, a second relaunch of every activity): WM's sync engine then stayed busy past the 1.5 s
            // themed-redraw deadline and every switch waited for the daemon's 3 s fail-open (6 Oct 17:01:44-47). It is
            // applied by the 500 ms bookkeeping right after the switch; the e-ink then shows one more update.
            if (!switching) mContrast.update(eink && !"lcd".equals(theme()));
            String animationState = eink ? "1" : "0";
            if (!animationState.equals(Dualux.get(Dualux.NO_ANIMATIONS, ""))) Dualux.set(Dualux.NO_ANIMATIONS, animationState);
            int slop = android.view.ViewConfiguration.get(mContext).getScaledTouchSlop();
            if (slop != Dualux.getInt(Dualux.GESTURE_SLOP, -1)) Dualux.set(Dualux.GESTURE_SLOP, Integer.toString(slop));
            if (mTheme == null) return false;
            int boot = Settings.Global.getInt(mContext.getContentResolver(), Settings.Global.BOOT_COUNT, -1);
            if (boot < 0) return false; // wait for Settings provider; do not guess across reboots
            mTheme.update(eink, theme(), boot);
            mReportedFailure = false;
            return true;
        } catch (RuntimeException e) {
            report(e);
            return false;
        }
    }

    /** Confirm this process has received the authoritative global night configuration.
     * A configuration match is not a promise that every other app has drawn. */
    boolean ready(boolean eink) {
        if (mTheme == null) return readyResult(false, "theme_api");
        try {
            if (eink && !mAnimations.suppressed()) return readyResult(false, "animation_settings");
            if (eink && !AnimationReadiness.authoritativeSuppressed()) return readyResult(false, "animation_wm");
            int overlay = (Integer) invoke(mOverlayGetter, mUiManager);
            if (overlay != ThemeOverride.OFF && overlay != ThemeOverride.LIGHT && overlay != ThemeOverride.DARK) return readyResult(false, "overlay_value");
            Class<?> am = Class.forName("android.app.ActivityManager");
            Object service = am.getMethod("getService").invoke(null);
            Configuration global = (Configuration) Class.forName("android.app.IActivityManager")
                    .getMethod("getConfiguration").invoke(service);
            int night = global.uiMode & Configuration.UI_MODE_NIGHT_MASK;
            if (night != (mContext.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)) return readyResult(false, "global_local_config");
            if (eink && !"lcd".equals(theme())) {
                int desired = "dark".equals(theme()) ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO;
                if (night != desired) return readyResult(false, "requested_config");
            }
            return readyResult(true, "ready");
        } catch (ReflectiveOperationException | RuntimeException e) {
            report(e); return readyResult(false, "api_error");
        }
    }

    String readyReason() { return mReadyReason; }

    private boolean readyResult(boolean ready, String reason) {
        mReadyReason = reason;
        return ready;
    }

    private void report(Exception e) {
        if (!mReportedFailure) Log.w(Dualux.TAG, "E-ink theme overlay unavailable", e);
        mReportedFailure = true;
    }

    private static Object invoke(Method method, Object target, Object... args) {
        try {
            return method.invoke(target, args);
        } catch (IllegalAccessException | InvocationTargetException e) {
            throw new IllegalStateException("Theme overlay API failed", e);
        }
    }
}
