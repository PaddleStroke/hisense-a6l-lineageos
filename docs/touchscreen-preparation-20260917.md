# Front touchscreen preparation

Prepared while the user was away. The spare remained in stock Android. No
reboots, image writes, driver binding, GPIO writes or controller-register writes
were performed. V44's SurfaceFlinger package remains staged and untested on the
phone; complete that visual check before combining graphics and touch.

## Hardware evidence

Read-only `getevent -lp` on the spare identifies the front input as `ft8719_ts`,
with INPUT_PROP_DIRECT, five type-B multitouch slots, X=0..1080 and Y=0..2340.
Its I2C path is `c178000.i2c/i2c-4/4-0038`. The rear is a separate `ft5x06_ts`,
five slots, X=0..720 and Y=0..1440, on `c1b7000.i2c/i2c-7/7-0038`.
Live stock sysfs name/modalias reads were denied; no permissions were changed.

Stock DT `firmware/extracted/stock-dtbo-20260914/stock-00-merged.dts` supplies:

| Item | Front LCD touch | Rear e-ink touch |
|---|---|---|
| Address | 0x38 on QUP4, c178000 | 0x38 on QUP7, c1b7000 |
| Bus pins | GPIO14 / GPIO15 | GPIO26 / GPIO27 |
| Interrupt | GPIO67, falling edge | GPIO73, falling edge |
| Reset | GPIO66 | GPIO65 |
| I/O power | PM660 L11, 1.78–1.95 V | Same PM660 L11 |
| Separate VDD in stock node | Absent | PM660L L3 |
| Upgrade IDs in DT | 0x87 / 0x19 | 0x54 / 0x2c |

These are stock wiring observations, not permission to issue firmware-update
commands. No touchscreen firmware update is needed for the planned initial test.
The rear chip's exact model is not established by its generic input name alone.

## Driver and built artifacts

The pinned Linux 7.2.3 source already implements `focaltech,ft8719` in
`drivers/input/touchscreen/edt-ft5x06.c`. The actual V38 configuration already has
I2C QUP, I2C char devices and evdev built in, and EDT_FT5X06 selected as a module.
Built that unmodified driver as `edt-ft5x06.ko`, with no module dependencies.
Its version string matches `7.2.3-a6l-probe+`.

Artifacts and reports: `firmware/extracted/touchscreen-prep-20260917/`.
The kernel Image, .config and Module.symvers hashes were unchanged by the build.
The real module loaded, registered its I2C driver, unloaded and removed that
registration under diskless QEMU with the exact V38 kernel. Six ABI/cleanup
checks passed. This does **not** emulate an FT8719 or prove touch input works.

## Wiring candidate and limitations

`device/hisense/a6l/kernel/a6l-front-touch.dtso` adds the stock PM660 L11 I/O rail,
front IRQ configuration and FT8719 device. `Prepare-TouchTree.py` applies this
offline to a copy of V38's captured bootloader-adjusted DT and removes QUP4's
DMA properties for initial short PIO transfers. It checks all 23 changed
properties are confined to the touchscreen bus, its IRQ pins, I/O rail and new
symbols. Existing USB, storage, display reservations and rear touch are unchanged.

The merged candidate is a review artifact, **not a boot image**. Future image
packaging must apply these edits to the source base/board overlay before the
bootloader's dynamic changes; do not flash the captured merged DT directly.

Initial candidate omits reset-gpios and wakeup-source. Stock specifically keeps
the integrated front touch/LCD reset high around LCD suspend. Unlike the Poco F1
example, we do not yet have a native panel driver to coordinate power/reset.
The optional upstream reset sequence would assert GPIO66 and then deassert it
after 5 ms. We must not assume that preserves the bootloader-initialized LCD.

Stock declares no separate front VDD regulator. Consequently the initial
candidate uses the driver's dummy VCC supply and relies on the preserved panel
power state; IOVCC is real PM660 L11. This is provisional bring-up, not finished
power management. If the controller is asleep after boot, identification may
fail and coordinated panel/reset work will be needed. No arbitrary voltage or
reset-pin workaround has been implemented.

Upstream FT8719 allows ten slots whereas stock exposes five. Both use type-B
events. Standard touchscreen-size 1080/2340 yields maxima 1079/2339; stock reports
inclusive maxima 1080/2340. Orientation and edge calibration need physical taps.
No axis inversion or swapping has been guessed.

## Recorder prepared

`tools/Capture-TouchEvents.py OUTPUT --seconds 20` runs on the laptop only after
the diagnostic has a functioning front input device. It requires authenticated
V38 root ADB and the expected kernel, chooses one direct-touch device by the
front geometry, and rejects ambiguity or the rear panel. It records bounded raw
getevent output, capabilities, multitouch frames and a JSON result. It does not
grab input, inject events, change touch settings or start the driver. No-touch,
dropped-event and out-of-bounds results are not reported as successful input.

`tools/Test-TouchCapture.py` passes ten offline checks covering front/rear
selection, both coordinate conventions, two fingers, releases, edge positions,
out-of-range slots/coordinates, lost events, idle capture and retained type-B
slot coordinates on tracking-ID reuse. Actual stock capability output is
archived under `research/touchscreen-prep-20260917/`.

## Next physical work

1. Finish the already-staged V44 SurfaceFlinger visual check.
2. Package a separately reviewed touch diagnostic with the candidate wiring;
   retain the known-good kernel and stock recovery restore route.
3. Capture I2C/driver identification, IRQ counters and input capabilities before
   asking for taps. Diagnose power or I2C failures from those logs first.
4. Record corners, centre, swipe and two-finger gestures for twenty seconds.
5. Connect validated events to an on-screen cursor, then Android's input stack.
   Front/rear routing, suspend, wake gestures and e-ink coordination remain later.

Primary references checked:

- [Upstream driver](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux.git/+/7dd38d9dd7a05329825fe2324d4d8e27ad4b3cec/drivers/input/touchscreen/edt-ft5x06.c)
- Pinned local `Documentation/devicetree/bindings/input/touchscreen/edt-ft5x06.yaml`
- Pinned local `arch/arm64/boot/dts/qcom/sdm845-xiaomi-beryllium-ebbg.dts`
  (same touch controller, different board wiring/panel; not a drop-in tree).
