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
| `ModeDecision_MirrorMode` (0x436b0) | `int f(struct buf* image, void* handle, int temp_a, int temp_b, int force, int mode)` | image = 1440×720×4 RGBA (`0x3F4800`). `temp_a/temp_b` = `this+0x594/0x590`, written by `GetEpdTopTemp/BottomTemp` just before. `force` = `this+0x598` (cleared afterwards), `mode` = `this+0x5a0` (requested refresh mode). Returns the mode actually chosen. |
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
