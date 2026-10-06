# Sending `hi846-set-ctrl.patch` upstream (linux-media)

**Patch:** `docs/upstream/hi846-set-ctrl.patch` ("media: i2c: hi846: Fix hi846_set_ctrl() returning 1 on success").

**Upstream status (checked 29 Sep 2026 on git.kernel.org):**
- The bug is still present in torvalds master and in linux-next `next-20260929`.
- The code is identical in both: `ret = pm_runtime_get_if_in_use(); if (!ret || ret == -EAGAIN) return 0;` and no reset afterwards.
- It was introduced in v6.5 by 04fc06f6dc15 ("media: hi846: fix usage of pm_runtime_get_if_in_use()"). Up to v6.4 the code was `if (!pm_runtime_get_if_in_use()) return 0;` with `ret = 0`.
- Since v6.5, `__v4l2_ctrl_handler_setup()` returns 1 in `hi846_start_streaming()`, so the sensor never streams on any mainline kernel. That is why the patch carries `Cc: stable`.

**Tested:**
- The patch applies with `git am` to linux-next and to our 7.2.3 tree, whose hi846.c is identical to master.
- checkpatch reports 0 errors. Its only warning is "unknown commit id", because the local tree is shallow; the id was verified on git.kernel.org.
- Phone evidence: attended t38 (camera17, `a6l_fix=1` is exactly this change). With the fix, 1632x1224, 1280x720 and 640x480 produced test-pattern and live frames; with `fix=0` the sensor stayed off.

## Steps

1. **Fill in your identity.** Replace `PIERRE_FULL_NAME <PIERRE_EMAIL>` in both the `From:` and the `Signed-off-by:` lines with your real name and email. The Signed-off-by is your DCO certification, so only you can add it.

2. **Rebase onto the media tree and re-run checkpatch:**
   ```
   git clone --depth 50 https://git.linuxtv.org/media.git && cd media   # or linux-next
   git am /path/to/hi846-set-ctrl.patch
   ./scripts/checkpatch.pl --strict -g HEAD
   ./scripts/get_maintainer.pl -f drivers/media/i2c/hi846.c
   ```

3. **Send it as plain text with git send-email.** Do not use a webmail client, because it breaks tabs.
   ```
   git send-email --to=<hi846 maintainer from get_maintainer> --to=linux-media@vger.kernel.org \
     --cc=sakari.ailus@linux.intel.com --cc=<the other get_maintainer entries> -1
   ```
   - The Hi-846 maintainer is Martin Kepplinger, the author of the driver and of 04fc06f6dc15. Sakari Ailus maintains the sensor drivers.
   - `Cc: stable@vger.kernel.org` is in the tag block, so the stable team picks it up once it lands in mainline. Do not add stable to `--cc` by hand.
   - Gmail SMTP works with an app password: `sendemail.smtpserver=smtp.gmail.com`, `smtpserverport=587`, `smtpencryption=tls`.

4. **After sending:**
   - Answer review on the list with a plain-text reply-all.
   - If a v2 is asked for, add `[PATCH v2]` and a changelog below the `---` line.
   - Track it on patchwork.linuxtv.org (project linux-media).
