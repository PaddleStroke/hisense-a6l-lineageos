# First LineageOS framework boot on the A6L hardware — 20 September 2026 (attended)

**Result: LineageOS 23-based framework booted on the spare A6L from RAM under the V68 diagnostic recovery
(Linux 7.2.3 V67 kernel). The user saw "Welcome to LineageOS" on the LCD, touch works, the setup wizard advances.**
Extremely sluggish (10–20 s per interaction). Stock Android and eMMC untouched; everything ran from tmpfs.

## Session sequence
1. V68 installed with the generated tools (write + full readback + poweroff, 30 s); stock Android verified.
2. V68 recovery booted: authenticated ADB (`HLTE730T-PROBE`), eMMC enumerates (as `mmcblk1` on this kernel), recovery
   partition hash read back on-device = V68.
3. **E-ink SPI NOR backed up**: JEDEC `c2 25 33` = Macronix MX25U4033E, 512 KiB, status 0x0c, four identical reads,
   SHA-256 `bcc829d17a01e648927e89a002a4da328ba774ca01a1d90d11e641b79f2f2de8`, VCOM digits → 2400 mV.
   Stored at `firmware/extracted/eink-spi-nor-20260920/epd-nor.bin` (not in git). gpio42 alone powers the flash.
4. **ADSP candidate A: PASS** — firmware authenticated, running after ~1 s, 20 s hold, clean stop; rpmsg channels
   `apr_audio_svc`, `apr_apps2`, `fastrpcglink-apps-dsp`, `glink_ssr` appeared. CX-vote/SMMU concerns did not bite.
5. Framework runs (payload = V70 r2 EROFS image, 1.7 GB pushed over ADB in 50 s):
   - run 1: full boot, then crash — data-dir helper still had an inline QEMU-only guard (fixed in launcher + harness);
   - run 2: failed early — BPF/cgroup leftovers of run 1; **a fresh recovery boot is required per run**;
   - run 3: full boot to WelcomeActivity in ~120 s; screen slept after 60 s while unattended; 20-min limit ended it;
   - run 4: with `edt-ft5x06.ko` loaded first → welcome screen seen by the user, touch working after fixes below.

## Problems found and live fixes (to fold into the harness)
| Problem | Live fix | Proper fix |
|---|---|---|
| EventHub `Permission denied` on the supervisor-created `/dev/input/event*` (0660 root:input) | re-created nodes 0666 inside the namespace via `nsenter` → devices added | create 0666 (diagnostic) or fix ownership/groups in supervisor |
| Backlight stuck at 256/4095: HWC cannot write because `/sys` is bound read-only | wrote brightness from outside | writable bind of the backlight directory only |
| After the 60 s screen timeout Android never woke: `setPowerMode(Off)` fails on simpledrm (atomic commit −EINVAL), power key events arrive but wake does not complete | installed `cmd`/`input`/`settings` into the overlay, `settings put system screen_off_timeout 2147483647`, `input keyevent 224` | ship those tools in the payload, set timeout before boot; make drm-hwc treat Off on simpledrm as a no-op/black frame |
| Very slow UI | — | **no cpufreq driver bound: all 8 cores sit at the bootloader frequency**; rendering is SwiftShader (CPU). Needs cpufreq for SDM660 + Adreno (candidate G1 + Mesa) |
| toybox in the recovery lacks `awk`, needs `mount -o rprivate`, `/tmp` is nodev | scripts adapted | keep scripts to the toybox subset; test with the recovery's toybox |

## Next
cpufreq/OPP for the Kryo 260 clusters, GPU bring-up, harness fixes above in the payload, then ADSP-dependent
audio/sensors and the e-ink drive-frame work using the captured waveform.
