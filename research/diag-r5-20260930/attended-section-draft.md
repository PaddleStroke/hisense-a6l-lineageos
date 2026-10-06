All commands on the laptop desktop session, `cd ~/A6L-usb-20260915/diag-r5`. Serial is `HLTE730T-PROBE` (state `recovery`)
in the V75 recovery AND in every diag image. Nothing here uses EDL. Never `rmmod msm`.

**0. Check the kit** (once): `sha256sum -c --quiet KIT-SHA256SUMS && echo KIT_OK` and `bash tests/test-bootwrite-mock.sh | tail -n 1`
(`BOOTWRITE_MOCK_PASS`).

**1. Write the main diag image into boot** (phone in the V75 recovery: `adb devices` → `HLTE730T-PROBE recovery`):
```
host/diag-bootwrite.sh images/diag-r5p-dt6b.img --expect-current 3616b0fa63e8af3928d1d5df146f73327620270d81198968c82f8140be0a2947
```
Expected: `boot = mmcblk1p29 dev 179:29 start 671744 size 131072`, `current boot sha256 3616b0fa…`, `backup saved logs/boot-before-….img`,
`pushed, phone sha ok`, `dd: …67108864 bytes…`, `readback sha256 9b9e8bb7…`, `BOOTWRITE_OK 9b9e8bb7… (previous 3616b0fa…)`.
Any `BOOTWRITE_FAIL` before the dd = nothing written. `READBACK MISMATCH` = do not reboot, rerun with the printed backup file.

**2. Boot it and confirm adb**
```
host/diag-stream.sh boot1 &                                   # stream starts itself when the phone appears
adb -s HLTE730T-PROBE shell 'echo b > /proc/sysrq-trigger'    # immediate reboot -> ABL -> boot partition = diag image
adb wait-for-recovery; adb -s HLTE730T-PROBE shell 'cat /proc/cmdline; uname -r' | grep -oE 'a6l_diag=[^ ]+|^7\.2\.3.*'
```
Expected within ~1-2 min: LCD shows the earlycon console as in the recovery; `a6l_diag=r5p-dt6b`, `7.2.3-a6l-probe+`.
`grep A6L_USBWD logs/boot1-*.log` shows the USB watchdog (as in V75). If no adb after 3 min: long-press Power, Power+Vol-down →
Recovery (V75, untouched) and restore (step 4); report the LCD's last lines.

**3. Bisect (one msm attempt per boot; reboot = `adb -s HLTE730T-PROBE shell 'echo b > /proc/sysrq-trigger'` if alive, else long-press Power)**
Every boot: `host/diag-stream.sh <name> &` (one per test; `tail -f logs/<name>-*.log` in a 2nd terminal), then `host/diag-push.sh`
(`DIAG_PUSH_OK 7x files`). Reading the stream: `A6L_DIAG <step> BEGIN insmod X` is printed before each insmod, `OK insmod X`
after; `A6L_DIAG hb <uptime>` every 2 s. Heartbeat stops + stream silent = hard freeze (note the last 30 lines). `STUCK insmod`
+ `STACK` + sysrq-w/l dumps = blocked but alive. `kernel BUG` / `list_add corruption` / `Unable to handle` = oops.
| test | commands (after push) | question |
|---|---|---|
| T1 | `host/diag-run.sh pre` (wait `DONE pre`), `host/diag-pull.sh T1-pre`, `host/diag-run.sh msm` | does msm freeze here? |
| T1+ (only if T1 alive) | `host/diag-run.sh panels`, `host/diag-pull.sh T1-after` | LCD/e-ink come up? |
| T2 | `host/diag-run.sh pre`, `host/diag-run.sh msm skip_gpu=1` | display part only |
| T3 | `host/diag-run.sh pre`, `host/diag-run.sh dispoff`, `host/diag-run.sh msm` | GPU part only |
| T4 | `DIAG_ENV=A6L_DIAG_NOKFENCE=1 host/diag-run.sh pre`, `host/diag-run.sh msm` | KFENCE sampling off |
| T5 (only if T1 alive) | `host/diag-run.sh rom`; next boot: `host/diag-run.sh ioload 180`, `host/diag-run.sh pre`, `host/diag-run.sh msm` | ROM order with base group; msm under eMMC load (r6b's mke2fs) |
| verbose (repeat the failing test once) | before msm: `adb -s HLTE730T-PROBE shell 'echo 0x1ff > /sys/module/drm/parameters/debug'`, then `host/diag-run.sh msm dyndbg=+p` | last driver step before the freeze |
After a freeze, long-press Power (reboots into the same image), then `host/diag-stream.sh <name>-post &`, `host/diag-push.sh`,
`host/diag-run.sh setup`, `host/diag-pull.sh <name>-pstore` (pstore-prev/console-ramoops-0 = the frozen boot's console, if DDR kept it).

**Kernel / DT bisect** (only if T1 freezes). Switch images from inside a diag boot (the eMMC driver is loaded at ~20 s) or from
the V75 recovery, same script, then reboot as above; run T1 each time (the kit picks the module set from the image tag):
```
host/diag-bootwrite.sh images/diag-v67-dt6b.img            # V67 kernel + ROM DT
host/diag-bootwrite.sh images/diag-r5m-dt6b.img            # r5 without the android config fragment
host/diag-bootwrite.sh images/diag-k1-dt6b.img             # r5p without KFENCE / BUG_ON_DATA_CORRUPTION
host/diag-bootwrite.sh images/diag-r5p-dtv74.img           # r5p on the V74 DT
```
Reading: v67-dt6b OK → the kernel (then r5m: OK → android fragment → k1: OK → KFENCE or BUG_ON (T4 tells which); k1 still
freezes → LIST_HARDENED/HARDENED_USERCOPY/rest → 3rd variant); v67-dt6b freezes → the ROM DT (r5p-dtv74 should pass) → DT
overlay bisect next.
Copy `logs/` to the desktop `captures/diag-r5-<date>/` at the end.

**4. Restore the r6b boot** (from a diag boot or the V75 recovery):
```
host/diag-bootwrite.sh ../rom-r6b/images/boot.img           # BOOTWRITE_OK 3616b0fa…
```
Then long-press Power; Power+Vol-down → Recovery = V75 as before (recovery, dtbo, system, vendor were never touched).
