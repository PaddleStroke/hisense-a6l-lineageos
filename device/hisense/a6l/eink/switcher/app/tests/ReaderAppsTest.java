// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

/** eink-round9: reader-sleep allowlist. */
public final class ReaderAppsTest {
    private static int checks;
    private static void check(boolean result, String name) {
        if (!result) throw new AssertionError(name);
        checks++;
    }
    public static void main(String[] args) {
        check(ReaderApps.allowed("org.koreader.launcher", "readers", ""), "KOReader is a reader");
        check(ReaderApps.allowed("org.readera", "readers", null), "ReadEra is a reader");
        check(!ReaderApps.allowed("com.android.chrome", "readers", ""), "Chrome is not");
        check(!ReaderApps.allowed("com.android.launcher3", "readers", ""), "the launcher is not");
        check(ReaderApps.allowed("com.android.chrome", "all", ""), "all apps");
        check(ReaderApps.allowed(null, "all", ""), "all apps, even unknown");
        check(!ReaderApps.allowed(null, "readers", ""), "unknown foreground: never with readers");
        check(ReaderApps.allowed("com.example.comics", "custom", "org.koreader.launcher, com.example.comics"), "custom list");
        check(!ReaderApps.allowed("org.koreader.launcher", "custom", "com.example.comics"), "custom replaces the defaults");
        check(!ReaderApps.allowed("com.example.comics", "custom", ""), "empty custom list: nothing");
        check(ReaderApps.allowed("org.koreader.launcher", "bogus", ""), "unknown mode = readers");
        check(ReaderApps.validList("org.koreader.launcher,com.example.x_y"), "valid list");
        check(ReaderApps.validList(""), "empty list is valid");
        check(ReaderApps.validList(" org.a.b , com.c.d "), "spaces allowed");
        check(!ReaderApps.validList("org.koreader.launcher;rm -rf"), "junk rejected");
        check(!ReaderApps.validList("nodots"), "a package has dots");
        check(!ReaderApps.validList("a.b\nc.d"), "newline rejected");
        StringBuilder big = new StringBuilder(); while (big.length() <= ReaderApps.MAX_LIST) big.append("com.a.b,");
        check(!ReaderApps.validList(big.toString()), "length bound");
        System.out.println("READER_APPS_PASS " + checks);
    }
}
