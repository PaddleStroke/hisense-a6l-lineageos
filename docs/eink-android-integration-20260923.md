# Rear e-ink in Android: how stock did it and a plan for LineageOS (23 Sep 2026)

Offline design only. Nobody touched the phone. Sources: disassembly of the stock `hwcomposer.sdm660.so` and
`libtcon_eink.so` (llvm-objdump), `firmware/extracted/eink-services-disassembly.txt` (EpdManagerService,
baksmali of `services.odex`), `references/Hisense_A6L_Eink_Display/Findings.md` and `epd_switch.sh`,
`docs/eink-swtcon-abi-20260919.md`, and the working V73 service (`device/hisense/a6l/diagnostic/a6l_epdd.c`,
v2 = `a6l_epdd_v2.c`, see `docs/eink-clear-prep-20260923.md`).

## 1. How stock Android 9 drove the rear screen

```
 Java  EpdManagerService (system_server, binder "epd", AIDL com.hmct.epd.IEpdManager)
        | setEpdDisplayMode / forceClear / setDisplayType / setExternalTpEnabled / addBitmapToPresentationDisplay ...
        v
 JNI   SurfaceControl.setEpdMode(int) / forceClearGhosting(int) / setDisplayType(int) / connectEpdDisplay(int)
        v   (custom ISurfaceComposer transactions in libgui, handled in the patched libsurfaceflinger.so)
 SF    writes sysfs: /sys/class/graphics/fb1/epd_display_mode, epd_force_clear, epd_display_type, epd_connect,
        /sys/ctp1/ctp_func/tpenable (rear touch on/off)
        v
 HWC   HWCDisplayExternalEpd (hwcomposer.sdm660.so), display type "external EPD", mirror mode:
        producer thread  EinkSwTconThreadHandler -> WaitForNextImage():
            image = the composed front screen, downscaled/rotated to 1440x720 RGBA (BitMapModeGetImage /
                    NormalModeWaitImage), temperatures from the TPS65185 (GetEpdTop/BottomTemp),
            ClearGhosting(): reads fb1/epd_force_clear; if non-zero -> force = 1 and writes "0" back,
            forceDisplayMode(): reads fb1/epd_display_mode -> mode (default 8 set in the constructor),
            n = ModeDecision_MirrorMode(image, handle, tempA, tempB, force, mode); force := 0,
            Update_Display_Image() into a 5-slot ring, n times.
        consumer thread  EpdUpdateThreadHandler: flips each ring slot to fb1 (DSI1 -> TC358767 -> panel), one per vblank.
```

Facts from the binaries:
- **Mode numbers used by stock**: EpdManagerService has `"fast:6"`, `"reading:3"`, `"picture:2"` (log "set e-paper
  mode:<name>:<n>", also per-app modes from `epd_mode_cfg.xml`, "DISPLAY_MODE_WITH_PRESENTATION restrict to fast",
  video/camera on -> fast). The HWC constructor sets the default mode to **8** before anything is written.
- **What the numbers mean inside libtcon_eink.so** (`ModeDecision_MirrorMode_Lib` dispatch at 0x3010, jump table 0x9778):
  0 = automatic (the library analyses histogram and changed area and picks GC16 / REGAL / DU / A2);
  1 = waveform 1 (DU-like, fewer greys, 23 frames); 2 = waveform 2 (GC16 full refresh, 38–39 frames);
  3, 4, 5 = waveform 5 (REGAL 16-grey partial, 39 frames); **anything > 5** (stock's 6 and 8) = waveform 6 (A2,
  black/white, ~10 frames). So stock "fast" = A2, "reading" = REGAL partial, "picture" = GC16.
- **Stock "clear ghosting"** (`EpdManagerService.forceClear` -> `SurfaceControl.forceClearGhosting` -> `epd_force_clear`)
  only set `force = 1` for one update: a forced GC16 redraw (38 frames) of the current picture. The long black/white
  INIT flash (98 frames) only ran on the first update after `Init_Eink_SWTcon` (library flag handle+0x270), i.e. at
  display power-on. a6l_epdd_v2 offers both (`refresh` = stock force clear, `clear` = INIT flash on demand).
- **Faces**: `setDisplayType` switched which screen is "on" (LCD vs e-ink face); `setExternalTpEnabled` writes
  `/sys/ctp1/ctp_func/tpenable`; the e-ink key is `KEY` code 616 (`KEY_LEFT_UP` in getevent naming, flip button);
  `com.hmct.einklauncher` was a separate home app for the e-ink face; `setDisplayType` also called
  `IActivityManager.beginDualScreenTask/finishDualScreenTask` (patched AMS) and kept per-app modes.
- The rest of the Java framework (power, display manager) was stock AOSP (Findings.md). Everything EPD-specific sat in
  EpdManagerService + patched SurfaceFlinger + the vendor HWC.

What we cannot reuse: the patched SF/libgui/AMS (Android 9 ABI), the fbdev HWC (msm_fb ioctls), the fb1 sysfs nodes
(4.4 msm_mdss driver). What we reuse unchanged: `libtcon_eink.so` (pure computation, already running on the phone
under V73) and the panel waveform from the SPI NOR.

## 2. What we have now (V73, proven 23 Sep)

- Kernel: `panel-a6l-epd-dsi.ko` brings the TC358767 bridge up in `.enable`; sysfs `epd_power` switches the TPS65185
  rails the stock way; the e-ink is a DRM connector 384x725 @ 85 Hz on its own CRTC.
- Userspace: `a6l_epdd` dlopens the stock `libtcon_eink.so`, turns PGM/PPM files into drive frames on the phone
  (0.14 s) and flips them one per vblank (~0.5–1.2 s per update). Commands via script or FIFO.
- In the Lineage UI boot from RAM the e-ink connector must NOT be claimed by drm_hwcomposer (it would try to use it as a
  second ordinary display). a6l_epdd must own that CRTC/connector (DRM master on a lease or a separate card node).

## 3. Plan for LineageOS

### (a) Vendor service `vendor.a6l.epd` (evolution of a6l_epdd)

- Binary `/vendor/bin/hw/a6l_epdd` (C, links libdrm statically, dlopens `/vendor/lib64/libtcon_eink.so`;
  waveform read once from the panel NOR by the kernel driver and exposed as a read-only sysfs/debugfs blob, or copied
  at first boot to `/mnt/vendor/persist/epd/epd-nor.bin`, never written back to the flash).
- API, phase 1: a Unix socket `/dev/socket/epd` (init `socket epd seqpacket 0660 system system`), line commands already
  implemented in `commands()`: `show <file> <mode>`, `clear`, `refresh`, `mode <n>`, plus new
  `frame <memfd>` (1440x720 RGBA or Y8 passed with SCM_RIGHTS, no file copy), `power off`, `status`.
  Phase 2 (for the framework): a stable AIDL HAL `vendor.hisense.epd.IEpd` (`showBuffer(HardwareBuffer, int mode)`,
  `clear()`, `refresh()`, `setMode()`, `setAutoMode(policy)`, `getState()`), registered as `vendor.hisense.epd.IEpd/default`.
- The service owns: the DRM connector/CRTC for the e-ink, the `epd_power` rails, XON, temperature, the library handle,
  and the "last image" (so `refresh` works and the panel can be redrawn after resume).
- Power: rails only during an update (already so); on screen-off of the e-ink face keep the last image (e-paper
  holds it without power) and drop the DSI1 stream (CRTC off) to save power; restart = modeset -> bridge bring-up.

### (b) Mirror mode (what stock did by default)

Source of pixels, from least to most invasive:
1. **Virtual display + ImageReader in a small system app/service** (`EinkMirror`, platform-signed, privileged):
   `DisplayManager.createVirtualDisplay("eink-mirror", 720, 1440, dpi, surface, VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR)`
   mirroring display 0 (portrait; a6l_epdd already accepts 720x1440 portrait input), `ImageReader` in RGBA_8888, max 2 images. Needs `CAPTURE_VIDEO_OUTPUT`
   (signature|privileged). Each new image -> hand the HardwareBuffer fd to the service (`frame <memfd>` / AIDL).
   This is the same data path screen recorders use; SurfaceFlinger composes it on the GPU (freedreno works).
   Portrait front (1080x2340) is letterboxed into the 720x1440 portrait e-ink (`AUTO_MIRROR` scales to fit).
2. Alternative without an app: a native helper using `SurfaceComposerClient::mirrorDisplay()` / `ScreenCapture`
   (the `screencap` path) at a timer; simpler to prototype from RAM (`screencap -p` ~100–200 ms) but polls.
3. Longer term: a real second display. Owning the e-ink as a normal display in drm_hwcomposer is the wrong model,
   because every present must go through the software TCON with a variable number of frames. Keep the service.

Automatic mode choice (in the service, all numbers in the library's terms):
- Compute a per-update changed-area ratio against the last image (downsampled 90x45 tiles, cheap).
- While content keeps changing (a new frame within 300 ms of the last, scroll/animation): **A2 (8)**, skip frames
  while an update is still being scanned out (latest-wins: keep only the newest pending frame).
- When content settles (no new frame for 500 ms): one **GC16 (2)** of the settled image ("picture"), or REGAL (3) if
  the change was small (< 15 % area) — REGAL is partial and ghost-light.
- Every N A2 updates (default 20) or every M REGAL updates (default 50), or when the user asks (e-ink key long press,
  quick-settings tile): **`refresh`** (forced GC16, 38 frames); `clear` (INIT flash) only on face switch to e-ink and
  on explicit request.
- Or delegate to the library: mode 0 (auto) already does a content-based choice; measure it in QEMU against the
  policy above before deciding (script-modes test in `docs/eink-clear-prep-20260923.md`).
- Per-app overrides like stock (`epd_mode_cfg.xml`): later, via the AIDL `setMode` from the app side.

### (c) E-ink key, faces, touch routing

- E-ink key = input code 616 on the gpio-keys node (seen as a working event in V46/V71). Map it in
  `/vendor/usr/keylayout/<device>.kl` to an unused Android key (e.g. `key 616 MACRO_1` / `STEM_PRIMARY`) and handle it
  in a small `EinkFaceService` (same app as the mirror) through a `KeyHandler` (Lineage `config_deviceKeyHandlerLib`
  / `org.lineageos.settings.device` pattern) — no framework patch needed.
- Face switch **to e-ink**: `echo 1 > /sys/ctp1/...tpenable` equivalent on the new kernel = enable the rear ft5x06
  input (inhibit the front one: `/sys/class/input/inputN/inhibited`, mainline supports input inhibit); turn the LCD
  backlight to 0 but keep display 0 logically ON (powering display 0 off would also stop the mirror source; powering
  the LCD panel down under a live display 0 is a later composer optimisation); start mirroring; one `clear`.
- The rear touch reports 720x1440; the mirrored front is 1080x2340 letterboxed. Route rear touches into display 0 with
  an input transform: an `/vendor/usr/idc/ft5x06_ts.idc` with `touch.deviceType = touchScreen` and
  `device.internal = 1`, associated to display 0 (`touch.displayId`/port association), then scale: 720x1440 panel ->
  the letterboxed area of 1080x2340 (letterbox scale 1440/2340 = 0.615, x offset (720-665)/2 = 27 px). Android's
  InputReader can only scale to the whole display, so either (i) make the mirror fill exactly (crop instead of
  letterbox: accept that the top/bottom 7 % are cut) or (ii) a tiny uinput bridge in the service that reads the rear
  evdev and injects transformed coordinates on a virtual touchscreen (full control, also lets us drop touches during
  A2 ghosts). Recommend (ii), it is 150 lines and keeps the kernel/IDC untouched.
- Face switch **to LCD**: inhibit rear touch, uninhibit front, restore brightness, stop mirroring (leave the last
  e-ink image or show a lock/"cover" picture — stock showed a wallpaper/clock via einklauncher).
- `wm size`/density changes (epd_switch.sh style) are optional: mirroring a 1080x2340 UI onto 720x1440 is readable at
  the default density; with 720x1440 and 16 greys a lower density (bigger UI) may read better; test both.

### (d) Init, SELinux, packaging

```
# device/hisense/a6l/epd/a6l_epdd.rc
service vendor.epdd /vendor/bin/hw/a6l_epdd --service --socket epd --waveform /mnt/vendor/persist/epd/epd-nor.bin
    class hal
    user system
    group system graphics input
    capabilities SYS_NICE
    socket epd seqpacket 0660 system system
    task_profiles HighPerformance
on post-fs-data
    mkdir /mnt/vendor/persist/epd 0750 system system
on boot
    chown system system /sys/bus/mipi-dsi/devices/<dsi1-dev>/epd_power
    chmod 0660 /sys/bus/mipi-dsi/devices/<dsi1-dev>/epd_power
```
- SELinux (vendor policy): new domain `hal_epd_default` (or `a6l_epdd`) with `init_daemon_domain`; `allow a6l_epdd
  gpu_device:chr_file rw_file_perms` (/dev/dri/card*), `sysfs_epd:file rw_file_perms` (label the epd_power,
  bringup_status and tps65185 hwmon nodes via genfs_contexts), `gpio_device` for gpiochip0 (XON; better: move XON into
  the panel driver so the service needs no GPIO), `vendor_file:file { read open getattr execute map }` for dlopen of
  libtcon_eink.so, `self:capability sys_nice`; socket type `epd_socket` with `unix_socket_connect(system_app, epd,
  a6l_epdd)`. For the uinput bridge: `uhid_device`/`uinput` access (`input_device` rw).
- drm_hwcomposer must skip the e-ink connector: patch `DrmDisplayPipeline` selection to ignore connectors named
  `DSI-2`/with mode 384x725, or give the service a **DRM lease** of that connector+CRTC+plane from the composer
  (cleanest upstream-style). Until then: start a6l_epdd before the composer and keep DRM master only on a second
  minor (`/dev/dri/card1` if the e-ink is split into its own drm device) — needs checking on the V73 kernel.
- Library + waveform: `libtcon_eink.so` from the stock vendor image into `vendor/hisense/a6l/proprietary/lib64/`
  (extract-files list), the waveform read from the panel NOR at runtime (read-only; never write the SPI flash).

## 4. Smallest first milestone testable from RAM

**M1 — "the Lineage UI shows up on the e-ink, refreshed on demand".**
In the existing LineageOS-from-RAM session (V71 supervisor, not real init yet):
1. Load bundle-r16 modules (V73 panel driver) before SurfaceFlinger starts and make sure drm_hwcomposer only takes the
   LCD connector (check `dumpsys SurfaceFlinger --display-id`: one display).
2. Start `a6l_epdd_v2 --fifo /tmp/epd/cmd` (as in r149) once the UI is up.
3. Loop in a shell on the phone: `screencap /tmp/epd/s.png` is PNG — use `screencap` raw (`screencap /tmp/epd/s.raw`
   gives 1080x2340 RGBA + 16-byte header); a 40-line helper `a6l_epd_grab` (NDK, static) converts raw -> 720x1440
   portrait PGM with letterboxing, then `echo "show /tmp/epd/s.pgm auto" > /tmp/epd/cmd`.
   Every 2 s, or when the image differs; `refresh` every 20 updates.
4. Pass: Pierre sees the Lineage home/settings on the rear screen, readable, updating within ~2–3 s of a change on the
   front, no speckles; `clear` works on demand (long flash each time).
M2 = same via a virtual display app + socket (event driven, no polling); M3 = rear touch via the uinput bridge +
e-ink key face switching; M4 = packaged vendor service under real init with SELinux enforcing.

Risks / unknowns: DRM ownership between drm_hwcomposer and a6l_epdd in the same boot (not tested yet); CPU cost of
screencap at 1080x2340 (~8 MB copy, fine at 0.5 Hz); the auto mode (0) behaviour on real UI content; whether the
r113-style stream non-recovery appears during long sessions; A2 ghosting accumulation rate on this panel.
