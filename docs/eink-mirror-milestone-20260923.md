# E-ink mirror, milestone M1: the rear e-ink shows the Lineage front screen (agent `eink2`, 23 Sep 2026)

Offline work only. **Nobody touched the phone.** Nothing here has run on the phone yet.
Builds on `docs/eink-android-integration-20260923.md` (M1 plan) and `docs/eink-clear-prep-20260923.md` (a6l_epdd_v2).

## Summary
- **Yes, the Lineage session would take the e-ink as a second display.** Two things stop a6l_epdd from driving it:
  - **Display claim.** drm_hwcomposer (`external/drm_hwcomposer`, `ResourceManager::UpdateFrontendDisplays`) attaches
    **every connected DSI connector** as an internal display. It does this at start-up and again on each hotplug uevent.
    No property excludes a connector: `vendor.hwc.drm.internal_display_names` only marks connectors as internal.
  - **DRM master.** The composer opens every `/dev/dri/card%` and calls `drmSetMaster`, then keeps master for the
    whole session (`ro.vendor.hwc.drop_drm_master` is not set, and it would break the composer's own commits).
    LCD and e-ink sit on the same msm card: `crtc-0` + plane-0 is the LCD, `crtc-1` + plane-1 is the e-ink. This is
    from the V71 debugfs dump `v71/logs/exp1/drm-state.txt`. So a6l_epdd_v2 would get EBUSY on `drmSetMaster` and
    EACCES on `SetCrtc`/`PageFlip`.
- **M1 solution, no Lineage-tree build needed:**
  1. **Hide the connector.** *Before* the framework starts, force the e-ink connector off:
     `echo off > /sys/class/drm/cardN-DSI-x/status`, run from the recovery shell. drm_hwcomposer then sees it as
     disconnected and never attaches it. The mode is saved first, because a forced-off connector reports no modes.
  2. **Lease it.** *After* the UI is up, `a6l_epdd_v3 --lease auto` does the following:
     - finds the process holding the DRM master file (the composer);
     - duplicates that fd with `pidfd_getfd()` (root);
     - calls `DRM_IOCTL_MODE_CREATE_LEASE` on it for {e-ink connector, its free CRTC, the primary planes that can feed
       that CRTC};
     - closes the duplicate, then works only on the lessee fd.

     The lessee is a master of its own lease, so v2's SetCrtc and page-flip code runs unchanged. The composer keeps
     full access to its LCD objects.
- **Mirror:** `a6l_eink_mirror` (C, NDK static):
  - **Capture.** Runs `screencap` (raw) *inside SurfaceFlinger's mount namespace*, via `setns` on
    `/proc/<sf>/ns/mnt` with SF's own environment. So it runs from the recovery shell next to the framework (private
    binderfs, bind-mounted `/system`). A second source reads the LCD planes directly (`--source drm`: GETFB2 +
    PRIME mmap, CAP_SYS_ADMIN, no master needed; linear buffers only).
  - **Conversion.** Area-averages 1080x2340 into a 720x1440 portrait PGM (white letterbox of 27 px each side, or
    `--fit crop`).
  - **Change detection.** Detects changes on 8x8 tiles.
  - **Mode policy.** Drives a6l_epdd through its FIFO with automatic modes (below). Only one command is outstanding at
    a time; completion is read from epdd's log.
- **Verified offline:**
  - a6l_epdd_v3 in QEMU `--dry` with the real waveform gives output **byte-identical to v2 for all 13 updates** of a
    mirror-style script: 3 mirror PGMs, quality / fastest / fast / refresh / clear.
  - The mirror policy was checked with an x86 build of the same source on 56 synthetic raw screencap frames.
  - Both NDK builds pass (0 errors).
- **Not tested at all:** the lease (pidfd_getfd + CREATE_LEASE on msm), the connector force-off, screencap through
  setns, the `drm` source, the arm64 mirror binary at runtime, and the e-ink modeset while the LCD is being driven by
  the composer.

## 1. What the Lineage session does with the e-ink (task 1)
- **Composer start-up.** `framework_root_services.c` starts `android.hardware.composer.hwc3-service.drm` with no hwc
  properties. The private `/dev` gets `card0` plus every `cardN`/`renderD*` from sysfs, and drm_hwcomposer's default
  pattern `/dev/dri/card%` opens them all.
- **Connector selection.** `DrmConnector::IsInternal()` covers `DRM_MODE_CONNECTOR_DSI`, and
  `UpdateFrontendDisplays()` binds every connected internal connector. So if the V73 panel module is loaded (connector
  connected with 384x725), SurfaceFlinger gets a second internal display. It would then flip ordinary RGB composition
  onto crtc-1. That is harmless to the ink, because the rails are only on inside a6l_epdd updates, but it blocks
  a6l_epdd.
- **Hotplug.** Loading the panel module *after* the composer does not help: the uevent listener re-runs
  `UpdateFrontendDisplays()`.
- **Master.** `DrmDevice::Init` calls `drmSetMaster` and fails without it. `DropDrmMasterAfterInit()` exists but would
  leave the composer unable to commit.
- **Exclusion options:**

  | Option | Needs a build? | Status |
  |---|---|---|
  | (a) sysfs `status=off` before the composer starts | no | **used in M1** |
  | (b) `video=DSI-2:d` on the kernel command line (same effect, from boot) | no | not tested |
  | (c) drm_hwcomposer patch with `vendor.hwc.drm.ignore_connectors=DSI-2` | yes (`m`, real-init agent) | sketch in `firmware/extracted/eink-mirror-20260923/drm_hwcomposer-ignore-connectors.patch`, not applied, not compiled; for M4 |
- **The lease is still needed with any of (a), (b) or (c),** because master stays with the composer.
- **Why not a lessee created by the composer?** drm_hwcomposer has no lease code. The whole tree was grepped for
  "lease": only fences. So v3 creates the lease itself from the composer's file.

## 2. What was built (task 2)

| Path | sha256 |
|---|---|
| `device/hisense/a6l/diagnostic/a6l_eink_mirror.c` (new) | `a2c71d5f42769cb7295f1d6be81d19ca98cb6153bc594d6f157e9c008119dfa9` |
| `device/hisense/a6l/diagnostic/a6l_epdd_v3.c` (new, = v2 + ~100 lines; v2 untouched) | `c1cac655de542d0a6b5726b8dfabe09245b1c71576b1092b824d98b7ab0e9bbb` |
| `device/hisense/a6l/diagnostic/eink-mirror-session.sh` (phone launcher) | `168d681e2a09a94c33776ee2be299b99d4a4648e7134fa17500b327903463f4e` |
| `firmware/extracted/eink-mirror-20260923/a6l_epdd_v3` (NDK r27c API 34, static libdrm, dynamic libc/libdl like v2) | `08777be3b6502e4ecf14ea4f5aef28cd342168d8d7a4354eef61b1583840fc1b` (the QEMU run used build `aee4930d…`; the only difference is a connector-type check (`DSI`) in the `--mode-file` path, which `--dry` does not use) |
| `firmware/extracted/eink-mirror-20260923/a6l_eink_mirror` (NDK r27c, fully static) | `a60014e12969d7f2080de29e2ccb0e5c51bfa213616af45e6ee3988dde9ce195` |
| laptop `~/A6L-usb-20260915/v74/eink-mirror/` (the 3 files above + `SHA256SUMS`, `sha256sum -c` OK on the laptop) | same |

### a6l_epdd_v3: changes from v2
- **`--save-mode F`**: find the connected 384x725 connector, write {connector id, drmModeModeInfo} to F, drop master,
  and exit. Use this before the connector is forced off.
- **`--mode-file F`**: use that connector id and mode even when the connector is disconnected.
- **`--lease auto|PID`**, only when our plain fd is not master:
  1. Enumerate the primary planes whose `possible_crtcs` include the chosen CRTC. The CRTC's own primary is among
     them. Per the V71 dump, plane-1 is crtc-1's primary.
  2. Scan `/proc/*/fd` for `/dev/dri/card*` links on the same device. `pidfd_open` + `pidfd_getfd` each one and try
     `drmModeCreateLease`; non-master files, such as minigbm's, fail with EACCES and are skipped.
  3. Switch to the lessee fd.

  If we are master ourselves (no composer), v3 behaves like v2.
- **Everything else is v2**: same start-up clear, `clear`, `refresh` and modes. QEMU proves the frames are identical
  (§4).

### a6l_eink_mirror: policy (defaults, all tunable)
- **Poll** every 250 ms. Compare tile means (8x8, threshold 6/255) with the previous capture (motion) and with the last
  picture sent (panel).
- **Discrete change** (panel differs, and the content has been quiet for 300 ms): `show <pgm> quality` (GC16, 39
  frames).
- **Burst** (changed in ≥ 2 consecutive captures): `show <pgm> fastest` (A2) whenever the previous command has
  finished and 150 ms have passed. The latest frame wins. `--active-mode fast` uses DU instead.
- **Settle** (no change for 1000 ms after a burst): one `quality` of the settled picture. If the settled picture is the
  one already sent in A2, it sends `refresh` instead, because the library skips an identical image with force=0.
- **Clear:** every 10th quality update is `clear` (the v2 full flash, 2 updates) followed by the quality picture.
- **Rate limits:** ≤ 60 updates/min, 150 ms minimum gap, one command outstanding.
- **Completion:** epdd log lines "shown in" or FAIL, with a 12 s timeout. Without `--epdd-log`, fixed estimates are
  used (0.55 / 0.85 / 2.0 s).
- **Landscape:** a landscape capture (w > h) gives a 1440x720 PGM. epdd accepts that; its orientation on the panel is
  unverified.
- **Mode-switch cost, measured in QEMU:** the first A2 after a quality is a 39-frame mode change, and later A2 updates
  are 10 frames. `refresh` after A2 is 79 frames. `fast` after quality is 62 frames. So a burst costs one 39-frame
  update before A2 gets fast, which is expected.

## 3. Launcher and attended test procedure (task 3)
`eink-mirror-session.sh` runs in the **recovery** shell, outside the framework namespace (there, `/sys` is writable
and `/dev/dri` + gpiochip get mknod'ed). Subcommands: `pre`, `start`, `status`, `clear`, `refresh`, `stop`,
`restore`.

Attended steps (main agent + Pierre; fresh V71 recovery boot; use `export PATH=/tmp/bin:$PATH` in every `ph.sh`):
1. **E-ink bring-up**, exactly as for r149/r155: bundle-r16 modules loaded, `bringup_status` shows `bridge_ok=1`.
   Push the e-ink files:
   ```
   lap: cd ~/A6L-usb-20260915 && (cd v74/eink-mirror && sha256sum -c SHA256SUMS) && adb -s HLTE730T-PROBE push v73b/epd /tmp/ && adb -s HLTE730T-PROBE push v74/eink-mirror/a6l_epdd_v3 v74/eink-mirror/a6l_eink_mirror v74/eink-mirror/eink-mirror-session.sh /tmp/epd/
   ```
2. **`ph.sh 60 'export PATH=/tmp/bin:$PATH; sh /tmp/epd/eink-mirror-session.sh pre'`**
   - Expect: `e-ink connector: /sys/class/drm/cardN-DSI-2 status=connected`,
     `wrote mode file ... 384x725@85`, `after force-off: status=disconnected`, `MIRROR_PRE_PASS`.
   - Pierre: the e-ink should not change. Watch dmesg for a panel unprepare (rails off, stock order); nothing should
     happen on the LCD.
3. **Start the LineageOS session** exactly as in the V71 UI run (framework payload, `A6L_EGL=mesa`, display modules).
   Wait for the Lineage UI on the LCD.
   - Check `logs/composer.log` (after the run): "Attaching connector DSI-1" only, **no DSI-2**.
4. **`ph.sh 120 'export PATH=/tmp/bin:$PATH; sh /tmp/epd/eink-mirror-session.sh start'`**
   - Expect:
     - `DRM user:` lines (composer + allocator);
     - `pid <composer> fd N (/dev/dri/cardN): master, lease granted` and `lease <id> created with k objects`;
     - `DRM connector=… crtc=…`;
     - `bring-up 1: … bridge_ok=1`;
     - update 1 = 99 frames and update 2 = 78 frames (the start-up clear);
     - `serving /tmp/epd/cmd`;
     - mirror `surfaceflinger pid …`, `frame 1: 1080x2340 capture NNN ms`, `cmd: show … quality`, `done in ~0.9 s`.
   - Pierre: a long flash, then the Lineage home screen on the rear panel (portrait, camera on top, white 27-px side
     bars), readable, and **the LCD keeps working normally** (no freeze or flicker when the e-ink CRTC is enabled).
5. **Interaction tests.** Pierre does each; the timings are expectations.

   | Pierre does | Expected on the e-ink |
   |---|---|
   | Opens Settings | Quality update ≤ ~1.5 s after the tap |
   | Scrolls a long list for 3 s | Black/white A2 updates about 2 per second, then one clean quality update ~1 s after stopping |
   | Uses it for 10+ quality updates | One `clear` flash appears |
   | Rotates to landscape (optional) | Report how it looks |

   Then `sh … status`: updates ok, `missed-vblank-lines=0`.
   - `sh … clear` and `sh … refresh` by hand work as in r155.
6. **Stop.** `sh … stop` (mirror killed, epdd `quit`: the rails are off, the CRTC is off). End the framework as usual.
   Then `sh … restore` (connector "detect").
   - Ask Pierre: readable? latency? ghosting after bursts? any speckles? LCD unaffected?

**Fallbacks during the session**

| Failure | Fallback |
|---|---|
| Lease fails with EPERM (pidfd_getfd blocked, e.g. SELinux) | Check `getenforce` in recovery. If enforcing, stop there and report. Do not change policy without Pierre. |
| screencap fails (`WARN screencap failed`) | `MIRROR_ARGS="--source drm"`. It works only if the log does not say `modifier … compressed`. |
| Composer attached DSI-2 anyway (composer.log) | Stop. The e-ink CRTC is in use by SF. |
| The LCD glitches when the e-ink modeset happens | `sh … stop`, and report it. |

## 4. Offline verification
- **QEMU `--dry`, real waveform** (`tools/Test-EpddQemu.py`, runs `epdd-qemu-20260923-rmv2` / `-rmv3b`, PGMs
  generated by the mirror). Script: `quality a`, `quality b`, `fastest s`, `fastest a`, `fastest b`, `refresh`,
  `clear`, `quality s`, `fast a`, `quality a`.
  - Result: `passed: true`, 13 updates.
  - **All 13 `.a6lepd` files are sha-identical between v2 and v3.** Update 1 = `d4b2cb7a…`, the same start-up INIT as
    rc01.
  - Frame counts: 99, 78, 39, 39, 39 (switch to A2), 10, 10, 79 (refresh), 39+99 (clear), 39, 62 (switch to fast),
    39.
- **Mirror logic** (x86 build of the same source, `--dry`, 56 synthetic 1080x2340 RGBA frames: static A → B → 15
  scrolling frames → static):
  - Start → `quality`. The change to B → one `quality` (no fast).
  - Scrolling → `burst start`, 5× `fastest` about every 0.55 s (one outstanding).
  - Settle → `clear` + `quality` (with `--clear-every 3`) and `burst end (settled 1379 ms)`.
  - The PGM is 720x1440 P5 with exactly 27 white columns on each side.
- **Builds:** v3 and the mirror build with the NDK, 0 errors (style warnings only). qemu-user is not installed in WSL,
  so the arm64 mirror binary did not run anywhere.
- **Lineage screencap source** (`frameworks/base/cmds/screencap/screencap.cpp`): the raw header is w, h, format,
  dataspace = 16 bytes, rows written without stride. The mirror derives the header size from the length (12 or 16).
  With more than one display and no `-d`, screencap warns and takes the first display; with the connector hidden there
  is one display.

## 5. Risks and unknowns
- **Lease.** It depends on:
  - `pidfd_getfd` being allowed (root plus the recovery's SELinux state);
  - msm accepting a lease with {connector, crtc-1, primary planes};
  - legacy SetCrtc on the lessee finding crtc-1's primary in the lease.

  All are standard kernel paths, but none has run on this kernel.
- **E-ink modeset from a disabled CRTC while the LCD is live.** After `status=off`, fbcon should disable crtc-1, so
  epdd's SetCrtc is a real enable and runs the V73 `.enable` bring-up: PHY reset plus the INTF2-paused table, with 3
  retries. So far bring-up has only run at module load, before INTF2 ran. There is a risk of the r113-style stream
  non-recovery, and of disturbing the LCD. Watch the LCD and `bringup_status`.
- **Speed.** screencap at 1080x2340 on SDM660 through freedreno is unmeasured; ~100–300 ms plus process start is
  assumed, which is fine at 4 Hz polling. The CPU cost of polling (fork, screencap, 10 MB pipe) is unknown; raise
  `--interval` if the UI stutters.
- **Other unknowns:**
  - rotation of landscape content on the panel;
  - A2 ghost accumulation between clears on real UI content;
  - `--source drm` probably fails if minigbm/freedreno scan out UBWC buffers.
- **Composer restart.** If the composer restarts, the lease is revoked and epdd updates fail (it logs FAIL and keeps
  going). Restart with `stop` + `start`.
- **M4 (packaged).** Replace the sysfs trick with `vendor.hwc.drm.ignore_connectors` (patch sketch) or a lease API in
  the composer; replace screencap polling with a VirtualDisplay/ImageReader app (M2 in the integration doc).
