// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/**
 * E-ink settings (Settings > Display > E-ink display, or long-press a Quick Settings tile). Plain framework widgets, no
 * AndroidX, so it builds with javac against any android.jar. Every control writes a property; the daemon (a6l_dualux)
 * and the mirror (vendor.a6l_eink) pick changes up live (<= 1 s).
 */
public class SettingsActivity extends Activity {
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private LinearLayout mRoot;
    private TextView mScreenState;
    private Button mSwitch;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        ScrollView sv = new ScrollView(this);
        mRoot = new LinearLayout(this);
        mRoot.setOrientation(LinearLayout.VERTICAL);
        int p = dp(16);
        mRoot.setPadding(p, p, p, p);
        sv.addView(mRoot);
        setContentView(sv);
        build();
    }

    @Override protected void onResume() { super.onResume(); refreshScreen(); }

    private void build() {
        header(R.string.sec_screen);
        mScreenState = text("");
        mSwitch = new Button(this);
        mSwitch.setOnClickListener(v -> {
            Dualux.request(Dualux.isEink() ? "lcd" : "eink");
            mHandler.postDelayed(this::refreshScreen, 600);
        });
        mRoot.addView(mSwitch);
        text(getString(R.string.keys_help));

        header(R.string.sec_refresh);
        RadioGroup rg = new RadioGroup(this);
        String cur = Dualux.refreshMode();
        for (String m : Dualux.MODES) {
            RadioButton rb = new RadioButton(this);
            rb.setText(Dualux.modeLabel(m));
            rb.setId(View.generateViewId());
            rb.setTag(m);
            rg.addView(rb);
            if (m.equals(cur)) rb.setChecked(true);
        }
        rg.setOnCheckedChangeListener((g, id) -> Dualux.set(Dualux.P_REFRESH, (String) g.findViewById(id).getTag()));
        mRoot.addView(rg);

        header(R.string.sec_clear);
        Button clear = new Button(this);
        clear.setText(R.string.clear_now);
        clear.setOnClickListener(v -> Dualux.request("clear"));
        mRoot.addView(clear);
        TextView ceLabel = text("");
        int ce = Dualux.getInt(Dualux.P_CLEAR_EVERY, Dualux.DEFAULT_CLEAR_EVERY);
        seek(30, ce, ceLabel, v -> v == 0 ? getString(R.string.clear_never) : getString(R.string.clear_every, v),
                v -> Dualux.set(Dualux.P_CLEAR_EVERY, Integer.toString(v)));

        TextView ctLabel = text("");
        seek(100, Dualux.getInt(Dualux.P_CONTRAST, 0), ctLabel, v -> v == 0 ? getString(R.string.contrast_off) : getString(R.string.contrast, v),
                v -> Dualux.set(Dualux.P_CONTRAST, Integer.toString(v)));

        header(R.string.sec_frontlight);
        toggle(R.string.fl_enable, Dualux.getInt(Dualux.P_FL_ENABLE, 1) != 0, on -> Dualux.set(Dualux.P_FL_ENABLE, on ? "1" : "0"));
        TextView flLabel = text("");
        seek(100, Dualux.getInt(Dualux.P_FL_MAX, 100), flLabel, v -> getString(R.string.fl_max, v),
                v -> Dualux.set(Dualux.P_FL_MAX, Integer.toString(v)));
        toggle(R.string.fl_curve, "2".equals(Dualux.get(Dualux.P_FL_GAMMA, "1")) || "2.0".equals(Dualux.get(Dualux.P_FL_GAMMA, "1")),
                on -> Dualux.set(Dualux.P_FL_GAMMA, on ? "2" : "1"));

        header(R.string.sec_keys);
        RadioGroup kg = new RadioGroup(this);
        RadioButton ks = new RadioButton(this), kc = new RadioButton(this);
        ks.setText(R.string.ekey_sleep); ks.setId(View.generateViewId());
        kc.setText(R.string.ekey_clear); kc.setId(View.generateViewId());
        kg.addView(ks); kg.addView(kc);
        ("clear".equals(Dualux.get(Dualux.P_EKEY, "sleep")) ? kc : ks).setChecked(true);
        kg.setOnCheckedChangeListener((g, id) -> Dualux.set(Dualux.P_EKEY, id == kc.getId() ? "clear" : "sleep"));
        mRoot.addView(kg);

        header(R.string.sec_other);
        toggle(R.string.mirror_in_lcd, Dualux.getInt(Dualux.P_MIRROR_LCD, 0) != 0, on -> Dualux.set(Dualux.P_MIRROR_LCD, on ? "1" : "0"));
        toggle(R.string.per_screen, Dualux.getInt(Dualux.P_PER_SCREEN, 1) != 0, on -> Dualux.set(Dualux.P_PER_SCREEN, on ? "1" : "0"));
    }

    private void refreshScreen() {
        boolean running = Dualux.daemonRunning(), eink = Dualux.isEink();
        mScreenState.setText(running ? getString(eink ? R.string.screen_eink : R.string.screen_lcd) + "  (" + Dualux.state() + ")"
                : getString(R.string.daemon_missing));
        mSwitch.setText(eink ? R.string.switch_to_lcd : R.string.switch_to_eink);
        mSwitch.setEnabled(running);
    }

    // ---- tiny widget helpers ----
    interface IntFmt { String f(int v); }
    interface IntSink { void set(int v); }
    interface BoolSink { void set(boolean v); }

    private int dp(int v) { return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()); }

    private void header(int res) {
        TextView t = new TextView(this);
        t.setText(res);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        t.setPadding(0, dp(20), 0, dp(6));
        mRoot.addView(t);
    }

    private TextView text(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setPadding(0, dp(4), 0, dp(4));
        mRoot.addView(t);
        return t;
    }

    private void toggle(int res, boolean on, BoolSink sink) {
        Switch s = new Switch(this);
        s.setText(res);
        s.setChecked(on);
        s.setPadding(0, dp(8), 0, dp(8));
        s.setOnCheckedChangeListener((CompoundButton b, boolean v) -> sink.set(v));
        mRoot.addView(s);
    }

    private void seek(int max, int value, TextView label, IntFmt fmt, IntSink sink) {
        SeekBar sb = new SeekBar(this);
        sb.setMax(max);
        sb.setProgress(Math.max(0, Math.min(max, value)));
        label.setText(fmt.f(sb.getProgress()));
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int v, boolean user) { label.setText(fmt.f(v)); }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) { sink.set(s.getProgress()); }
        });
        mRoot.addView(sb);
    }
}
