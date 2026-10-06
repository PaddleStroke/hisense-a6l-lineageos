// SPDX-License-Identifier: Apache-2.0
package org.lineageos.a6l.dualux;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.MediaStore;
import android.widget.Toast;

/**
 * eink-lockscreen: no UI of its own (not in the launcher). Started by Settings > Display > E-ink ("Choose the picture"):
 * opens the system photo picker, stores the chosen picture as the e-ink lock-screen background (LockBackground) and
 * finishes. Protected by WRITE_SECURE_SETTINGS like the settings provider.
 */
public class LockImagePickActivity extends Activity {
    private static final int PICK = 1;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (b == null) {
            Intent pick = new Intent(MediaStore.ACTION_PICK_IMAGES).setType("image/*");
            try {
                startActivityForResult(pick, PICK);
            } catch (RuntimeException e) {
                startActivityForResult(new Intent(Intent.ACTION_GET_CONTENT).setType("image/*").addCategory(Intent.CATEGORY_OPENABLE), PICK);
            }
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        Uri uri = data == null ? null : data.getData();
        if (request != PICK || result != RESULT_OK || uri == null) { finish(); return; }
        new Thread(() -> {
            boolean ok = LockBackground.fromUri(getApplicationContext(), uri);
            runOnUiThread(() -> {
                Toast.makeText(getApplicationContext(), ok ? R.string.lock_pick_done : R.string.lock_pick_failed, Toast.LENGTH_SHORT).show();
                finish();
            });
        }, "a6l-lock-picture").start();
    }
}
