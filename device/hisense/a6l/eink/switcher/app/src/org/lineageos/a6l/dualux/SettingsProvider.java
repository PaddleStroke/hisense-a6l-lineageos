// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;

/** Settings uses the controller process, keeping appearance preferences in one owner. */
public final class SettingsProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }

    @Override public Bundle call(String method, String arg, Bundle extras) {
        getContext().enforceCallingOrSelfPermission(
                "android.permission.WRITE_SECURE_SETTINGS", "Privileged e-ink settings");
        if ("set".equals(method)) {
            String value = extras == null ? null : extras.getString("value");
            if (!SettingsValues.valid(arg, value)) throw new IllegalArgumentException("Invalid e-ink setting");
            if ("theme".equals(arg)) PerScreenAppearance.get(getContext()).setTheme(value);
            else Dualux.set(SettingsValues.property(arg), value);
        } else if ("request".equals(method)) {
            if (!"eink".equals(arg) && !"lcd".equals(arg) && !"clear".equals(arg)) {
                throw new IllegalArgumentException("Invalid display request");
            }
            Dualux.request(arg);
        } else if (!"get".equals(method)) {
            throw new IllegalArgumentException("Unknown settings operation");
        }
        Bundle result = new Bundle();
        result.putString("state", Dualux.state());
        result.putString("theme", PerScreenAppearance.get(getContext()).theme());
        for (String key : SettingsValues.KEYS) {
            result.putString(key, Dualux.get(SettingsValues.property(key), SettingsValues.defaultValue(key)));
        }
        return result;
    }

    @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) { return null; }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u, ContentValues v) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri u, String s, String[] a) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) { throw new UnsupportedOperationException(); }
}
