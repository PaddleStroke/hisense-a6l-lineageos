# Android side: e-ink settings, tiles and daemon wiring (29 Sep 2026)

Offline work. No phone, no adb, `m` not run (the r5 build was running in the tree).

## What was already there, and is in r5
The dualux agent (25 Sep, docs/dualux-20260925.md) already shipped `A6LDisplaySwitcher`. It is a privileged, platform-signed app in system_ext (`org.lineageos.a6l.dualux`). The r5 pipeline log shows `ok system:system_ext/priv-app/A6LDisplaySwitcher`. It provides:
- **Settings screen**: Settings > Display > "E-ink display". You can also open it by long-pressing any of its tiles. It covers:
  - active screen, with a switch button
  - refresh mode: auto / quality / reading / fast / fastest
  - "clear now", plus automatic clear every N updates
  - text contrast
  - frontlight: on/off, maximum %, gamma 2
  - what the e-ink key does
  - mirror on the e-ink while the LCD is in use
  - brightness and timeout remembered per screen
- **Quick Settings tiles**:
  - E-ink / LCD: switches the active screen
  - Refresh mode: cycles through the modes
  - Clear e-ink
  - Frontlight on/off
- **Wiring**: only properties cross the system/vendor boundary (no sockets or binder from the app).
  - Requests go through `sys.a6l.dualux.req`. Settings are the `persist.sys.a6l.{dualux,eink}.*` properties (type `a6l_dualux_ctl_prop`, system_ext; `set_prop(system_app, …)`).
  - The screen state is read from `vendor.dualux.state` (vendor_restricted).
  - `a6l_dualux` consumes the frontlight, key, mirror_in_lcd and request settings. It also writes `vendor.eink.clear_req` and `persist.vendor.eink.mode` for the mirror.
  - `a6l_eink_mirror` reads the refresh mode, clear_every and contrast live. The mirror talks to `a6l_epdd` over its socket.
- **Wiring into the build**: `rom/rom.mk` inherits `eink/switcher/dualux.mk` (PRODUCT_PACKAGES: app, daemon, privapp xml). `rom/BoardConfig-rom.mk` adds the vendor and system_ext sepolicy dirs.
- **No warmth control**: the frontlight is a single white LED on LPG channel 4 (see the DT overlay comment). The UI now says so.

## Changed today (app only; no native code or sepolicy changes)
- **French translation**: `app/res/values-fr/strings.xml`, all 48 strings, with the same format arguments as English.
- **Tile text now comes from string resources**, so it follows the language:
  - The refresh tile subtitle used a hard-coded English name (`shortName`). It now uses `short_*` resources.
  - The screen tile shows "Unavailable" when the daemon is down.
  - The frontlight tile shows "On, up to N %" or "Off".
  - The settings screen shows the state line from `screen_now`, and a frontlight help line (follows the brightness slider, no warmth).
- **Denied property read removed**: the app no longer reads `persist.vendor.eink.reading`. That property is vendor_internal, so system_app gets an avc denial when reading it; this was the "not fixed (minor)" item from the eink bug hunt. An unset or unknown refresh mode now shows as "auto", which matches what the mirror does with the build defaults.
- **New tests**:
  - `eink/switcher/app/tests/check-app.py`: 40 static checks. It checks en/fr key and format parity, escaped apostrophes, string, drawable and component references, and that no UI text is a Java literal. It checks that every property the app writes has the ctl_prop label and is used by a daemon, and that the app reads no vendor_internal property. It also checks the rom.mk → dualux.mk → PRODUCT_PACKAGES chain, the Android.bp module, the sepolicy dirs and the privapp permissions. **Result: CHECK_APP PASS 40/40.**
  - `eink/switcher/app/tests/build-check.sh <tree>`: aapt2 compile and link (the `fr` configuration is linked), `javac -Xlint:all -Werror` against the tree's system android.jar, and d8 via r8.jar. It writes only to /tmp. **Result: BUILD_CHECK PASS (classes.dex 26780 B).**
- `rom/tests/test-rom-static.sh`: PASS. The sepolicy check was not needed because no policy changed.

These changes reach the ROM in the next build (after r5). r5 has the English-only app.

## Attended tests after the first install
1. Settings > Display shows "E-ink display" (in French: "Écran e-ink"). The screen opens, and the state line shows `lcd`.
2. Add the 4 tiles in QS edit mode. Tap E-ink / LCD: the phone moves to the e-ink and `vendor.dualux.state=eink`. Tap it again to return to the LCD. Long-press a tile: the settings screen opens.
3. Refresh tile: the subtitle cycles through the modes. `getprop persist.sys.a6l.eink.refresh` follows it, and the mirror log shows the mode change.
4. Clear tile and the "clear now" button: a full flash on the e-ink.
5. Frontlight: on the e-ink, move the brightness slider; the level changes (`/sys/class/leds/epd-backlight/brightness`). Test the maximum % and gamma. The tile turns the light off and on. The light is off on the LCD.
6. Turn on "mirror on the e-ink while using the LCD": the LCD content appears on the rear screen.
7. Set the system language to French: all strings in the screen and tiles are in French.
8. Run `logcat | grep -i avc` while using the screen: no denial for `system_app` on `a6l_dualux_ctl_prop` or `vendor_dualux_prop`.
