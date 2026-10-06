// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.app.WallpaperManager;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ImageDecoder;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * eink-lockscreen: the background picture of the e-ink lock screen (drawn by the vendor daemon a6l_einklock while the
 * phone sleeps on the e-ink). The picture is centre-cropped to the panel's 1:2 portrait shape, scaled to 720x1440,
 * converted to grey and stretched between its 1st and 99th percentile (e-paper has a short tonal range); a6l_epdd dithers
 * it to the panel's 16 levels. Only properties and init cross the system/vendor boundary: the PGM is written to
 * /data/misc/a6l_eink/lock_bg.pgm (this app, system_app) and copied to /data/vendor/a6l_eink_lock by init when
 * sys.a6l.eink.lock_bg_seq changes (system_ext rc a6l_eink_lock.rc), which then sets vendor.dualux.lock_bg_seq.
 */
final class LockBackground {
    static final int W = 720, H = 1440;
    static final String SEQ = "sys.a6l.eink.lock_bg_seq";
    private static final File DIR = new File("/data/misc/a6l_eink");
    private LockBackground() { }

    /** A picture chosen in the photo picker. Returns false if it cannot be decoded or stored. */
    static boolean fromUri(Context context, Uri uri) {
        try {
            Bitmap b = ImageDecoder.decodeBitmap(ImageDecoder.createSource(context.getContentResolver(), uri), (decoder, info, src) -> {
                decoder.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
                int s = Math.max(1, Math.min(info.getSize().getWidth() / W, info.getSize().getHeight() / H));
                decoder.setTargetSampleSize(s);
            });
            return store(b, "image");
        } catch (IOException | RuntimeException e) {
            Log.w(Dualux.TAG, "lock screen picture " + uri, e);
            return false;
        }
    }

    /** The LCD lock-screen wallpaper (else the home wallpaper). False for a live wallpaper or no wallpaper access. */
    static boolean fromLcdWallpaper(Context context) {
        WallpaperManager wm = WallpaperManager.getInstance(context);
        Bitmap b = null;
        for (int which : new int[] {WallpaperManager.FLAG_LOCK, WallpaperManager.FLAG_SYSTEM}) {
            try (ParcelFileDescriptor fd = wm.getWallpaperFile(which)) {
                if (fd != null) b = BitmapFactory.decodeFileDescriptor(fd.getFileDescriptor());
            } catch (IOException | RuntimeException e) {
                Log.w(Dualux.TAG, "wallpaper file " + which, e);
            }
            if (b != null) break;
        }
        if (b == null) {
            try {
                Drawable d = wm.getDrawable();
                if (d instanceof BitmapDrawable) b = ((BitmapDrawable) d).getBitmap();
            } catch (RuntimeException e) {
                Log.w(Dualux.TAG, "wallpaper drawable", e);
            }
        }
        return b != null && store(b, "lcd");
    }

    /** Grey 720x1440 bytes (centre crop, filtered scale, luminance, percentile stretch). Package-private for tests. */
    static byte[] toGrey(Bitmap src) {
        int sw = src.getWidth(), sh = src.getHeight(), cw = sw, ch = sh;
        if ((long) sw * H > (long) sh * W) cw = (int) ((long) sh * W / H); else ch = (int) ((long) sw * H / W);
        Rect from = new Rect((sw - cw) / 2, (sh - ch) / 2, (sw - cw) / 2 + cw, (sh - ch) / 2 + ch);
        Bitmap dst = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
        new Canvas(dst).drawBitmap(src, from, new Rect(0, 0, W, H), new Paint(Paint.FILTER_BITMAP_FLAG | Paint.DITHER_FLAG));
        int[] px = new int[W * H];
        dst.getPixels(px, 0, W, 0, 0, W, H);
        dst.recycle();
        int[] hist = new int[256];
        byte[] grey = new byte[W * H];
        for (int i = 0; i < px.length; i++) {
            int p = px[i], a = p >>> 24;
            int y = (77 * ((p >> 16) & 255) + 150 * ((p >> 8) & 255) + 29 * (p & 255)) >> 8;
            y = (y * a + 255 * (255 - a)) / 255;	/* transparent = white paper */
            grey[i] = (byte) y; hist[y]++;
        }
        int lo = percentile(hist, px.length / 100), hi = percentile(hist, px.length - px.length / 100);
        if (hi - lo >= 32 && (lo > 0 || hi < 255)) {
            for (int i = 0; i < grey.length; i++) {
                int v = ((grey[i] & 255) - lo) * 255 / (hi - lo);
                grey[i] = (byte) Math.max(0, Math.min(255, v));
            }
        }
        return grey;
    }

    private static int percentile(int[] hist, int rank) {
        int n = 0;
        for (int v = 0; v < 256; v++) { n += hist[v]; if (n > rank) return v; }
        return 255;
    }

    private static synchronized boolean store(Bitmap b, String kind) {
        byte[] grey = toGrey(b);
        int seq = (int) (System.currentTimeMillis() / 1000L);
        File tmp = new File(DIR, "lock_bg.pgm.tmp"), out = new File(DIR, "lock_bg.pgm");
        try (FileOutputStream f = new FileOutputStream(tmp)) {
            f.write(("P5\n# a6l-lock seq=" + seq + "\n" + W + " " + H + "\n255\n").getBytes(StandardCharsets.US_ASCII));
            f.write(grey);
            f.getFD().sync();
        } catch (IOException e) {
            Log.e(Dualux.TAG, "lock screen picture not written to " + DIR, e);
            return false;
        }
        /* init refuses group/world-writable sources: keep the default 0600 (umask) */
        if (!tmp.renameTo(out)) { Log.e(Dualux.TAG, "rename " + tmp + " failed"); return false; }
        Dualux.set(Dualux.P_LOCK_BG, kind);
        Dualux.set(SEQ, Integer.toString(seq));	/* init copies it for the vendor daemon */
        Log.i(Dualux.TAG, "lock screen background " + kind + " stored (seq " + seq + ")");
        return true;
    }
}
