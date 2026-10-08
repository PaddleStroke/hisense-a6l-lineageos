// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/** eink-round9: which foreground apps may arm reader sleep (persist.sys.a6l.eink.reader_apps). Pure, host-tested. */
final class ReaderApps {
    /** Common e-book / PDF readers (package names). */
    static final Set<String> DEFAULT = new HashSet<>(Arrays.asList(
            "org.koreader.launcher", "org.koreader.launcher.fdroid",
            "com.amazon.kindle",
            "com.flyersoft.moonreader", "com.flyersoft.moonreaderp",
            "com.google.android.apps.books",
            "org.readera", "org.readera.premium",
            "com.github.axet.bookreader",
            "com.foobnix.pro.pdf.reader", "com.foobnix.pdf.reader",
            "com.kobobooks.android",
            "com.obreey.reader",
            "org.geometerplus.zlibrary.ui.android",
            "com.aldiko.android",
            "com.adobe.reader",
            "com.xodo.pdf.reader"));
    static final int MAX_LIST = 1000;

    private ReaderApps() { }

    /** mode: "all" | "readers" (default) | "custom"; custom: comma-separated package names. */
    static boolean allowed(String pkg, String mode, String custom) {
        if ("all".equals(mode)) return true;
        if (pkg == null || pkg.isEmpty()) return false;
        if ("custom".equals(mode)) return parse(custom).contains(pkg);
        return DEFAULT.contains(pkg);
    }

    static Set<String> parse(String list) {
        Set<String> out = new HashSet<>();
        if (list == null) return out;
        for (String s : list.split(",")) { String t = s.trim(); if (!t.isEmpty()) out.add(t); }
        return out;
    }

    /** A custom list: package names (letters, digits, '_' and '.') separated by commas and spaces, at most MAX_LIST chars. */
    static boolean validList(String list) {
        if (list == null || list.length() > MAX_LIST) return false;
        for (String s : list.split(",", -1)) {
            String t = s.trim();
            if (t.isEmpty()) continue;
            if (!t.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)+")) return false;
        }
        return true;
    }
}
