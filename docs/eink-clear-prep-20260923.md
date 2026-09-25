# E-ink on-demand clear + mode map: preparation report (agent `eink`, 23 Sep 2026)

Offline work only: disassembly and QEMU. **Nobody touched the phone.** Nothing here has been shown on the panel yet.
Companion design doc: `docs/eink-android-integration-20260923.md`.

## Summary
- **Why a later `clear` gave only 38 frames.** In the stock `libtcon_eink.so`, `force=1` never meant "flash-clear".
  It means "update even if the image is unchanged, as a full GC16" (39 frames). The long black/white INIT flash
  (99 frames) runs only while the handle flag **+0x270 == 1**. Only `Init_Eink_SWTcon` sets that flag.
  `ModeDecision_MirrorMode` clears it on its 2nd call.
- **Stock never did the long flash on demand either.** Stock "clear ghosting" (`epd_force_clear`) = `force=1`: a GC16
  redraw of the current picture. The INIT flash only happened when the HWC initialised the library.
- **a6l_epdd_v2** adds a real on-demand INIT clear by setting the flag, and keeps the library state consistent.
  QEMU with the real waveform:
  - Every `clear` gives the same 99-frame INIT sequence as start-up, byte for byte, every time (tested 8 times,
    back to back included).
  - The next picture is byte-identical to the V73 picture sequence after start-up (the one r149 showed on the panel).
- **Mode map, from the disassembly and checked in QEMU:**
  - 0 = automatic;
  - 1 = DU-like;
  - 2 = GC16 ("picture");
  - 3/4/5 = REGAL partial ("reading" = 3);
  - any value > 5 = A2 black/white. Stock "fast" = 6; the stock HWC default is 8. 6, 7 and 8 give identical frames.

## 1. Disassembly findings (`firmware/extracted/vendor/lib64/libtcon_eink.so`, llvm-objdump)

`ModeDecision_MirrorMode` (0x2930), with `h` = the handle:
- It stores tempA/tempB at `h+0x102d8/0x102dc`.
- **If `h+0x274` (call counter) != 0 and `h+0x270` != 0:**
  - calls `Reset_Panel(DataInRGBABuf, 0x3f4800, 0xff)`, which is a memset: the library's input image becomes white
    and the caller's image is ignored;
  - clears `+0x270`, but only when the counter == 1;
  - sets `force` (`h+0x102e0`) to 0;
  - calls the Lib function.
- **Counter == 0 (first call):**
  - `Read_Config_FILE`, then builds the per-temperature waveform tables with `generate_wf`. The frame-count table
    fields and their waveform modes:

    | Field | Waveform mode |
    |---|---|
    | `+0xa0` | 2 |
    | `+0xa4` | 6 |
    | `+0xa8` | 5 |
    | `+0xac` | 1 |
    | `+0xb0` | 100 |
    | `+0xb4` | 0 |

  - then takes the same `+0x270` branch.
- **Otherwise:**
  - stores `force`;
  - `memcmp`s the new image against the previous one. If they are equal and `force == 0`, it logs "SDM epd image
    repeat" and sets `h+0x280 = 1`;
  - copies the image into `DataInRGBABuf`;
  - regenerates the waveform tables at most every 20 s, and only when the temperature bucket changed;
  - calls `ModeDecision_MirrorMode_Lib(DataInRGBABuf, h, mode)`;
  - then increments the counter `+0x274`.

`ModeDecision_MirrorMode_Lib` (0x2fa0):
- Mode dispatch (jump table 0x9778):

  | Mode argument | Internal mode |
  |---|---|
  | 0 | content analysis (0x3040): the result maps to internal 2 (GC16), 5 (REGAL), 6/8 (A2, threshold 0.05) or 1 |
  | 1 | 1 |
  | 2 | 2 |
  | 3, 4, 5 | 5 |
  | > 5 | 8 |

- `+0x270 == 1` forces internal mode 0 (INIT waveform, count `+0xb4`).
- `force` (`+0x102e0`) → `+0x278 = 2`: a full GC16 "mode-change" refresh with count `+0xa0` − 1 = **38**. This is the
  38-frame "clear" V73 saw.
- Counter == 1 (the 2nd call) also forces the GC16 refresh. That is why start-up call 2 is a full white refresh.

`Update_Display_Image_Lib`:
- If `+0x270 == 1`, it generates INIT frames from the waveform-mode-0 table, over the whole panel, ignoring the image.
  These frames do **not** update the library's "previous image" state.

`Set_OneMode_Refresh`, `Add_OneMode_Count`, `GetRefreshFlg`, `Reset_ModeCount` (= the `ModeChangeRefresh` alias):
- Per-mode counters at `h+0x290…` decide automatic mode-change refreshes. For example, `GetRefreshFlg` becomes true
  after more than 80 updates in one mode, or when the A2 count hits 101.
- Nothing in them re-arms INIT, and the HWC never calls them.

Stock HWC (`hwcomposer.sdm660.so`, `HWCDisplayExternalEpd::WaitForNextImage` at 0x43568):
- `ClearGhosting()` (0x413d4) reads `/sys/class/graphics/fb1/epd_force_clear`. If the value is non-zero it sets
  `force = 1` and writes `0` back.
- `forceDisplayMode()` (0x41670) reads `/sys/class/graphics/fb1/epd_display_mode` into `mode`. The constructor
  default is **8**.
- It then calls `ModeDecision_MirrorMode(image, handle, top, bottom, force, mode)` and clears `force`.

Stock framework (`eink-services-disassembly.txt`):
- EpdManagerService mode names: `"fast:6"`, `"reading:3"`, `"picture:2"`.
- `forceClear()` → `SurfaceControl.forceClearGhosting(int)` → `epd_force_clear`.

## 2. What changed: `device/hisense/a6l/diagnostic/a6l_epdd_v2.c`

The file is new, copied from the current `a6l_epdd.c`, which is untouched. The diff is 143 lines; `diff -u` shows
all of it. The changes:

1. **Handle access.** `H_INIT_FLAG(h)` = `h+0x270` and `H_CALLS(h)` = `h+0x274` (u32).
2. **`clear_init()`.** It first runs a sanity check: flag ∈ {0,1} and the library's call counter == our update
   count. If the check fails, it logs `WARN handle layout unexpected` and does a force clear instead.
   Otherwise it:
   1. fills the image with white;
   2. sets `+0x270 = 1`;
   3. runs the update (mode 2, force 0), which gives 99 INIT frames;
   4. **sets `+0x270 = 0` again**. The library only clears the flag itself on call 2, so without this every later
      update would flash.
3. **`clear` variants** (`clear [full|stock|init|gc]`):
   - `full` (the default, also used by `--clear-every`) = forced white GC16 from the current picture (39 frames),
     then INIT (99 frames): about 138 frames, ~1.6 s at 85 Hz. Each waveform starts from the state the library
     assumes.
   - `stock` = INIT, then a white GC16: the order of calls 1+2 on a fresh handle.
   - `init` = INIT only. **Test use only**: it leaves the library believing the old picture is still displayed
     (QEMU proof below).
   - `gc` = the V73 behaviour: force=1 on white, 39 frames.
4. **`refresh`** = stock `epd_force_clear`: a forced GC16 redraw of the last shown picture (kept in `last_img`). Every
   `show` copies its image there.
5. **Mode names:**
   - `auto` = 0;
   - `picture` = `quality` = 2;
   - `reading` = `partial` = 3;
   - `fast` = 1;
   - `fastest` = `a2` = **8** (was 6; the frames are identical);
   - numbers pass through unchanged.
6. **Start-up is unchanged** (`run_update(1, 0, white)` then the reset to white). It logs
   `after start-up: init flag=0 calls=2`.

## 3. QEMU verification (real waveform `eink-spi-nor-20260920/epd-nor.bin`, 25 °C, `tools/Test-EpddQemu.py`)

Runs: `firmware/extracted/epdd-qemu-20260923-rc01clear`, `-rc01modes`, `-rc03clear2`. All checks passed: exit 0, no
FAIL, nothing refused.

| Test (update numbers) | Frames | Result |
|---|---|---|
| Start-up INIT (1) | 99 | sha `d4b2cb7a…`: 21 frames black, 4 idle, 21 white, 5 idle, 21 black, 4 idle, 21 white, 2 idle (two balanced black/white cycles) |
| Every later `clear init` / INIT part of `clear` (rc01: 4, 7, 12, 14, 15; rc03: 5, 8, 10) | 99 | **byte-identical to update 1, every time, also back to back** |
| Picture after start-up (landscape, mode 2) | 39 | `5bf785ee…` = V73 runs r1–r3 = what r149 showed |
| Picture after `clear init` only (rc01 5) | 39 | `ee223a71…` ≠ `5bf785ee` (= "landscape over landscape", as `refresh`): **stale state, do not use alone** |
| Picture after `clear stock` (rc01 9), `clear full` (rc01 13), default `clear` (rc03 6), `clear gc` (rc01 18) | 39 | all `5bf785ee…`: **state identical to start-up** |
| White GC16 part of the clears (rc01 8, 11; rc03 4, 9) | 39 | 18 frames all-black, 18 all-white, 3 idle |
| `refresh` of the landscape (rc01 16) | 39 | forced GC16 of the current picture |
| Modes 6 / 7 / 8 (rc01modes 4–8) | 10 | 6, 7 and 8 byte-identical (A2). The first update after a mode switch = 39 (mode-change refresh) |
| Mode 0 `auto` on landscape/gradient/portrait | 39 each | the library chose a 16-grey waveform for all three test pictures |
| Mode 3 | 39 | REGAL partial, as in r147 |

Notes:
- The `ModeDecision` return value is not the frame count after a mode change: `decision=137` gave 99 frames, and 77
  gave 39. a6l_epdd already loops on `Update_Display_Image` until it returns 0, so this is harmless.
- The `eink-swtcon-20260923-r4/update3` file differs from epdd's update3 only because r4 used the probe's own test
  pattern. epdd's update3 matches all earlier V73 epdd runs.

## 4. Artifacts

| Path | sha256 |
|---|---|
| `device/hisense/a6l/diagnostic/a6l_epdd_v2.c` | `b20e547c602cc69ea7c3ecb8c926c5a3613e65506a37e4ffde13edfb0786ef62` |
| `firmware/extracted/eink-clear-20260923/a6l_epdd_v2` (NDK r27c, API 34, static libdrm, stripped; needs libc/libdl only; 0 warnings) | `1d262a0034063d2e9a29061ea04c9eae27881109e8249fa19d03a88bfb7a1cf8` |
| laptop `~/A6L-usb-20260915/v74/eink/a6l_epdd_v2` + `SHA256SUMS` (verified on the laptop) | same |
| `firmware/extracted/eink-clear-20260923/script-phone-clear.txt`, also on laptop `v74/eink/` | phone test script |
| `firmware/extracted/eink-clear-20260923/eink-phone-clear-test.sh` | attended relay template (NOT in inbox) |
| `firmware/extracted/epdd-qemu-20260923-rc01clear`, `-rc01modes`, `-rc03clear2` | QEMU frames + reports |

Superseded: the build in rc01 (`be4f8c44…`) had `stock` as the default clear.

## 5. Attended test (main agent + Pierre; phone in V71 recovery with the bundle-r16 modules loaded as for r149)

1. Bring-up exactly as r149: V73 panel module loaded, `bringup_status` shows bridge_ok.
2. Copy `firmware/extracted/eink-clear-20260923/eink-phone-clear-test.sh` to `.relay/inbox/<new-name>.sh`. It:
   - checks the recovery is present;
   - verifies `v74/eink/SHA256SUMS` on the laptop;
   - pushes `v73b/epd` (library, waveform, test PGMs) plus `a6l_epdd_v2` and `script-phone-clear.txt` to `/tmp/epd`;
   - runs `a6l_epdd_v2 --script script-phone-clear.txt` (about 90 s).
3. Expected log: updates 1–2 (start-up: 99 + 78 frames) and `after start-up: init flag=0 calls=2`. Then:

   | Step | Command | Updates / frames |
   |---|---|---|
   | 1 | landscape quality | 39 |
   | 2 | clear | **39 + 99** |
   | 3 | gradient quality | 39 |
   | 4 | clear | 39 + 99 |
   | 5 | clear | 39 + 99 |
   | 6 | portrait quality | 39 |
   | 7 | refresh | 39 |
   | 8 | landscape fastest | 39 (mode switch) |
   | 9 | gradient auto | 39 |
   | 10 | clear | 39 + 99 |
   | 11 | landscape quality | 39 |

   No `WARN handle layout`, no FAIL, and `missed vblanks during drive=0` on every update.
4. **Ask Pierre:**
   - Did **each** `clear` (4 of them) show the long flashing sequence, i.e. one black/white flash and then two more
     black/white cycles, about 1.6 s, ending pure white?
   - Was the panel clean white after each one, with no speckles?
   - Did the picture after a clear show no trace of the previous picture?
   - Did `refresh` do one black/white flash and redraw the same portrait?
   - Did `fastest` show black/white only?
5. If Pierre saw it: repeat once (rule), then make `clear` = `full` the service behaviour in the integration work.

## 6. Risks and unknowns
- **Untested on the panel.** The INIT frames are byte-identical to the start-up clear that r131/r149 showed cleanly,
  so the drive data itself is proven. The new parts are running it several times in one session and the extra
  rail/heat load. There is no VCOM or rail change.
- The flag poke depends on this exact library build (sha `4eaf6536…5b3c` per docs/eink-swtcon-abi-20260919.md).
  A different library could mislay the offsets. The sanity check (flag ∈ {0,1}, counter == our updates) falls back
  to a force clear.
- A white GC16 from a stale state (`stock` order) drives an already-white panel through black: harmless in QEMU
  terms. `full` avoids it, so it is the default.
- Unknown: what `auto` (0) picks on real UI content (the synthetic tests always gave 39 frames); the long-term A2
  ghosting rate on this panel; temperature dependence (all tests ran at 25 °C, and the waveform tables are rebuilt
  only when the temperature bucket changes, at most every 20 s).
