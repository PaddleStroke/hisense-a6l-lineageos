# Stock software-TCON ABI recovered from the HWC callers — 19 September 2026

Offline static analysis only (llvm-objdump of `vendor/lib64/hw/hwcomposer.sdm660.so`, SHA-256
`d1dfac49…c4db`, and `vendor/lib64/libtcon_eink.so`, `4eaf6536…5b3c`). No phone access.
Addresses are link-time virtual addresses (HWC file offset = vaddr − 0x5000).

## Why this matters

`libtcon_eink.so` is a **pure software timing controller**: its only imports are libc
memory/string/time functions, `property_get`, `__android_log_print`, and `system`/`popen`. It opens no
device, issues no ioctl and maps nothing. It turns an RGBA image plus the panel's waveform into a
sequence of panel *drive frames*; the HWC then pushes each drive frame to the second DSI output.
That split means the proprietary part can be reused unchanged in a modern userspace (dlopen already
verified, V-eink ABI test of 17 September) and the part we must write ourselves is "present a
768×1450 8-bit buffer on the e-ink DSI path at a fixed cadence", i.e. an ordinary DRM/KMS problem.

`Set_VCOM` and `Read_Temperture` are 4-byte stubs in this build; the `system("echo %d > …")` /
`popen` strings point at MediaTek-style paths (`/sys/devices/bus.2/11008000.I2C1/…/1-0048/`) that do
not exist on the A6L and are not reachable from the five functions the HWC uses. The HWC does VCOM
(`i2c 2-0068/vcom`) and temperature itself.

## The complete interface used by the stock HWC

Only six library functions are called from the HWC (call sites in parentheses):

| Function | Recovered prototype | Notes |
|---|---|---|
| `Request_ProcessBuf_Size` (0x420ec) | `struct{u32 w; u32 h;} f(cfg*)` | constant: returns w=384 (0x180), h=725 (0x2d5); argument ignored. HWC ignores the result and hard-codes `0x10FE00` = 768×1450 = (2·384)×(2·725). |
| `Init_Eink_SWTcon` (0x421ac) | `void* f(struct buf ring[5], int count=5, u32 cfg[3], void* flash, u32 flash_len=0x70080, void* info)` | returns the handle (non-NULL = success) stored at `this+0x570`. No `malloc` import: the handle and all work buffers are static `.bss` (~75 MB). |
| `ModeDecision_MirrorMode` (0x436b0) | `int frames = f(struct buf* image, void* handle, int temp_a, int temp_b, int force, int mode)` | image = 1440×720×4 RGBA (`0x3F4800`). `temp_a/temp_b` = `this+0x594/0x590`, written by `GetEpdTopTemp/BottomTemp` just before. `force` = `this+0x598` (cleared afterwards), `mode` = `this+0x5a0` (requested refresh mode). Returns the number of drive frames that follow (emulator-verified). |
| `Update_Display_Image` (0x43d44) | `u8 f(struct buf* ring_slot, void* handle)` | `memcpy(slot->data, handle[0x98], 0x10FE00)` then `Update_Display_Image_Lib`. Returns non-zero while more drive frames remain; 0 = sequence finished. |
| `SetEinkContrast` | `void f(int which, int value)` | tail-calls `SetImageContrast`: which=1,2,3 select three global contrast words. |
| `ReportEinkSWTconLibVersion` | `void f(u32* major, u32* minor)` | 2.2 (verified in QEMU on 17 September). |

`struct buf { void* data; u32 size; u32 pad; }` (16 bytes). `cfg = {1440, 720, 50}` (literal at
0x44b10 plus `mov w8,#50`); the third word is most likely the frame rate in Hz.

## Threading model in the stock HWC

- `EinkSwTconInit` (0x42078): allocates the five 0x10FE00 ring buffers at `this+0x4f8…0x538`, reads
  the 0x70080-byte SPI flash window, calls `Init_Eink_SWTcon`, sets VCOM, then allocates the image
  buffers: `0x3F4800` (1440×720×4), two `0x438000` (1440×768×4), another `0x3F4800`, and `0x1FA400`
  (1440×720×2).
- `EinkSwTconThreadHandler` (0x43c48, producer): when the previous sequence ended, `WaitForNextImage`
  fetches the next composed image, reads both temperatures, runs `ClearGhosting`/`forceDisplayMode`
  and calls `ModeDecision_MirrorMode`. It then calls `Update_Display_Image` into ring slot
  `(write+1) % 5`, blocking on a condition variable while the ring is full.
- `EpdUpdateThreadHandler` (0x43768, consumer): takes ring slots in order and presents them through
  `DrawEpd`/`GetFramebuffer`/`SetActiveFramebuffer` (framebuffer-style flips on the e-ink display).

## Emulator result (eink-swtcon-20260919-r2)

`tools/Test-EinkSwtconQemu.py 2` ran the full sequence in the diskless VM with a **zero-filled** waveform:
5/5 checks pass, no fault against end-aligned guard pages, no `system`/`popen` attempt.

- `Request_ProcessBuf_Size` → 384×725 as disassembled.
- `Init_Eink_SWTcon` returned a handle, left the `cfg` guard word intact and wrote bytes 0…195 of `info`
  with ASCII text fields (all `'0'` for zero data): a 15-byte field at +0, 31 bytes at +47, then more from
  +94. `info` is therefore a ≤196-byte record of waveform identification strings (what `SaveEpdInfo` stores).
  Init does **not** validate the waveform; a bad or missing capture would be accepted silently, so the
  capture must be verified independently (repeat-read hashes, header sanity) before use.
- `ModeDecision_MirrorMode(img, handle, 25, 25, force=1, mode=0)` returned **116**, and
  `Update_Display_Image` then returned non-zero exactly 116 times before returning 0: the return value is
  the **number of drive frames in the sequence**, not a mode id. At `cfg[2]=50` Hz that is a 2.3 s update,
  plausible for an initial/clearing waveform. Frames 0–5 were identical (expected with a null waveform).

## Consequences for the port

1. The modern replacement is a small daemon or HWC3 plug-in that owns the same two threads: producer
   = this library, consumer = DRM atomic flips of a 768×1450 R8 buffer on the e-ink connector.
2. **The real SPI waveform remains the blocker** for meaningful output: `Init_Eink_SWTcon` consumes
   the 0x70080-byte flash image. It must be captured once from stock Android (read-only path already
   mapped in `research/claude-eink`). Until then frames produced offline are not valid drive data.
3. `device/hisense/a6l/diagnostic/eink_swtcon_probe.c` + `tools/Test-EinkSwtconQemu.py` exercise exactly
   this call sequence in the diskless VM with end-aligned guard-paged buffers, refusing `system`/`popen`.
   With the real waveform file as argument the same probe will produce the true drive-frame sequence
   offline, which can be inspected before any panel is driven.
4. Still unknown: exact meaning of `temp_a/temp_b` units, the `info` structure written at `this+0x5a4`,
   the pixel encoding of drive frames (expected: 2 bits per pixel source-driver codes, 4 pixels/byte
   giving 360 data bytes + 24 control bytes per half-line), and the DSI/bridge timing that carries them.

## 20 September: real waveform + drive-frame format decoded (eink-swtcon r5, emulator)

Input: first 0x70080 bytes of the panel NOR captured on the phone (`firmware/extracted/eink-spi-nor-20260920`).
`Init_Eink_SWTcon` now fills `info` with `ED058TC7U2` and `320_R301_AFD521_ED058TC7U2_TC…`: the panel is an
**E Ink ED058TC7** (5.84", 720×1440), waveform file by TCL; NOR = Macronix MX25U4033E, VCOM −2.40 V.

**Drive frame = a 192 × 1450 XRGB8888 video frame** (768 bytes per row = 192 "pixels" × 4 bytes), exactly what an
LCD-style DSI video pipe would carry to the DSI→parallel-RGB bridge:

| byte in each 4-byte pixel | observed values | meaning |
|---|---|---|
| 0 ("blue"/first colour lane) | `00`, `55`, `aa` | **source-driver data, 4 EPD pixels × 2 bits**: `00` no drive, `01` = one polarity, `10` = the other. 180 data bytes × 4 = 720 pixels per line; 1440 data rows → 259 200 bytes (histogram matches exactly) |
| 1 (second lane) | `00 01 02 06 08 0a` | **gate/source control strobes** (start pulses, clock, latch/output-enable) at line/frame boundaries; 12 columns + 10 rows of blanking carry them |
| 2 | `00` | unused |
| 3 | `ff` | alpha/padding |

So the e-ink "display driver" needed on the modern kernel is an ordinary **DSI video-mode panel of 192×1450 at
cfg[2] = 50 Hz** behind the bridge; the TCON library output can be flipped to it unchanged as XRGB8888. No
per-pixel protocol has to be reimplemented. The first update after init is a fixed 116-frame clearing sequence
(alternating full-panel `aa`/`00`/… phases, identical with a zero waveform, i.e. built in); a second, different image
took 38 frames (`55` data = opposite polarity visible), which is where the waveform tables matter.

Open: DSI1 + bridge (Toshiba, per stock DT) register init and the exact video timings (porches/clock) from the
stock panel node `qcom,mdss_dsi_epd_eink_qhd_video`; TPS65185 rail sequencing around updates (candidate E1);
temperature input units for `ModeDecision_MirrorMode`.
