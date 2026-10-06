# Independent e-ink appearance — 2 October 2026

Pierre requests white wallpaper by default on the rear screen, optional separate
rear wallpaper, and a rear theme independent of LCD light/dark settings. These
were requested after r6h was installed; none are present in its images.

## Prepared theme candidate

The switcher now has Light (default), Dark and Follow LCD choices, translated
into French. `PerScreenAppearance` uses Android's in-memory Attention Mode theme
overlay. It never calls `setNightMode`, changes the night-mode schedule, or writes
the LCD's night-mode settings. This is an active-screen override on the existing
mirrored Android display, not two simultaneously different application layouts.

The exact local framework source checks its overlay for explicit light/dark,
automatic twilight and custom schedules. Its setter updates configuration
without persisting the user's mode. API:
[UiModeManager](https://android.googlesource.com/platform/frameworks/base/+/master/core/java/android/app/UiModeManager.java);
implementation:
[UiModeManagerService](https://android.googlesource.com/platform/frameworks/base/+/master/services/core/java/com/android/server/UiModeManagerService.java).

The app stores its original overlay before the Binder call, restores only the
overlay it owns when returning to LCD, retains newer external changes, and
discards stale sessions after OS reboot. Repeated state polls perform no writes.
Permission `MODIFY_DAY_NIGHT_MODE` is requested and allowlisted; no new property
or SELinux permission is needed by this candidate.

Verification: real controller regressions pass 31 checks for screen switches,
light/dark/follow changes, app restart, OS reboot, external overlay changes,
Binder/storage failures, unavailable APIs and idle polling. App static checks
pass 40/40. Isolated aapt2 English/French resources, javac with warnings as
errors, and D8 compilation pass. No ROM restaging, new image build or phone
mutation was performed.

The normal framework battery-saver path would force dark mode after applying
the day overlay. A small next-build patch exempts an explicitly selected DAY
overlay while retaining normal saver behavior for OFF/NIGHT overlays. It is
prepared separately, not applied to the active Android tree or installed image.
Its pipeline-normalized patch applies cleanly to the current framework. A
compiled fixture evaluates the actual source method over 24 saver/car/overlay
combinations: only the explicit DAY case changes, and OFF/NIGHT retain their
prior behavior. This is a focused logic check, not a framework build/boot pass.

Physical acceptance remains open: default light on rear with dark LCD, restored
LCD scheduling, current Settings/launcher/SystemUI, independently themed apps,
process restart and fast repeated screen changes. The battery-saver patch needs
integration/compilation and a physical light-mode comparison with saver enabled
before calling this requirement complete.
Attention Mode interactions also need checking on the actual ROM.

## Wallpaper integration still pending

Avoid repeated `WallpaperManager.setBitmap` swaps: these write wallpaper data,
can migrate a shared home/lock wallpaper, change crop/color metadata and can
disturb live wallpaper state. They would also add work during an already slow
screen switch. Preferred integration is a rendering override while the rear is
active, preserving LCD wallpaper data/component and rendering white by default.

Settings should offer White and Choose e-ink wallpaper, with a Reset to white
action. Cache a selected image once, scale/crop it to the rear surface, and keep
heavy decode/file work off the screen-switch path. The original LCD wallpaper,
including live wallpaper, must resume unchanged when returning to LCD. The
framework/engine integration and image picker are not implemented yet; there is
no nonfunctional wallpaper preference exposed in the app.

Acceptance: white rear home background with colored/live LCD wallpaper; optional
rear image survives reboot; LCD crop/lock linkage retained; dark icon/text
visibility on white; no extra flash cycle or measurable switch delay. Determine
whether lock-screen wallpaper needs the same rear override during these tests.

## r6i integration update

The light/dark/follow controls, permission allowlist and corrected framework
battery-saver patch have now compiled in r6i. The actual final EROFS images
match the new APK/framework outputs and contain the appearance resources and
requested/allowlisted permission. The actual compiled framework method contains
the DAY-overlay exception. Earlier isolated checks remain supporting evidence;
physical theme restoration and battery-saver behavior remain untested. r6i is
being staged and is not installed as of this note.

The Display page now has General/LCD/E-ink tabs, with the rear's detailed
refresh/frontlight/appearance entry in E-ink. White/custom wallpaper remains
pending. The stock input comparison suggests a properly fenced rear-size render
target as a longer-term design; an auto-mirrored virtual display would still
mirror LCD wallpaper, so it alone cannot implement independent rear wallpaper.
An own-content display additionally needs correct task/input/decor handling.
This is an architectural option, not a drop-in fix or included implementation.
Primary references:
[DisplayManager flags](https://developer.android.com/reference/android/hardware/display/DisplayManager)
and [AOSP system decorations](https://source.android.com/docs/core/display/multi_display/system-decorations).
