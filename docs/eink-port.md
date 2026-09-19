# A6L e-ink port: verified starting evidence

Source of observations: the spare HLTE730T's verified L1632.6.01.04 stock backup,
extracted with tools/Extract-StockFilesystem.py. No experimental sysfs writes
were used to obtain these findings.

## Layers that must work together

1. Kernel 4.4.153: embedded CONFIG_FB_HS_MDSS_EPD_PANEL=y; strings identify
   eink,ed052tc2 and 720x1440. The LCD is 1080x2340. Appended device trees and
   the embedded kernel configuration are available for further analysis.
2. Vendor HWC: hwcomposer.sdm660.so exposes HWCDisplayExternalEpd and methods
   including DrawEpd, CommitBitMap, EinkSwTconThread and GetEpdDisplayType.
   This is an active processing implementation, not a second ordinary monitor.
3. Vendor software TCON: libtcon_eink.so exports Init_Eink_SWTcon,
   Release_Eink_SWTcon, SetEinkContrast and ReportEinkSWTconLibVersion.
4. System graphics: libgui.so exports SurfaceComposerClient::setEpdMode(int),
   setDisplayType(int), and connectEpdDisplay(int). Stock libsurfaceflinger.so
   must be analyzed for the corresponding transaction/behavior implementation;
   absence of exported EPD symbol names does not mean the implementation is absent.
5. Framework: the stock Binder service inventory includes
   epd / com.hmct.epd.IEpdManager. Framework and services JARs are stripped of
   classes.dex, so their OAT/VDEX artifacts need deodexing for code analysis.

## Stock kernel interfaces

vendor/etc/init/hw/init.product.rc assigns access to:

- /sys/class/graphics/fb1/epd_info
- /sys/class/graphics/fb1/epd_contrast
- /sys/class/graphics/fb1/epd_black_threshold
- /sys/class/graphics/fb1/epd_white_threshold
- /sys/class/graphics/fb1/epd_display_type
- /sys/class/graphics/fb1/epd_display_mode
- /sys/class/graphics/fb1/epd_force_clear
- /sys/class/graphics/fb1/epd_connect
- /sys/class/graphics/fb1/epd_commit_bitmap
- /sys/ctp1/ctp_func/tpenable
- /sys/kernel/mirror/state

Stock also exposes epd_vcom, /dev/epd_flash and panel power/thermal controls.
These may involve panel calibration or persistent controller storage. They are
not part of an ordinary refresh/screen-switching API and must not be guessed at.
Do not carry the stock world-writable permissions into a new implementation.

## Implementation sequence

First map existing transaction IDs, arguments, ordering and HWC dependencies.
Then define a small controlled e-ink service with validated refresh modes,
screen-switch sequencing and input routing, using the existing vendor HWC/TCON
if its ABI can be supported. Implement a replacement only where evidence requires
it. Keep kernel/source work separate from framework/vendor compatibility work.
Validate rear touch, screen switching, contrast, partial/full refresh, frontlight,
orientation, lockscreen, sleep/wake and battery use separately on the spare.

The WanderingArrow reference is a useful lead, not a verified implementation:
its Android 11 ceiling claim and descriptions of unchanged framework classes
do not establish limitations of a source port. Its script has not been run.

Private symbol/dependency inventory: firmware/extracted/eink-elf-analysis.json.

## Stock SurfaceComposer transaction map (offline verified)

Inspected arm64 libgui.so SHA-256:
`68c215bc31fd45203a75ba1296a077734102eaea638ecae9ef14b48fa4596415`.
Addresses below are ELF virtual addresses before relocation, not file offsets.

| Method | Client wrapper | Virtual slot offset | Proxy implementation | Transaction | Server case |
| --- | --- | --- | --- | --- | --- |
| setDisplayType(int) | 0x98adc | 0x100 | 0x7e310 | 30 / 0x1e | 0x7b6b8 |
| setEpdMode(int) | 0x98c58 | 0x118 | 0x7e524 | 33 / 0x21 | 0x7b3d0 |
| connectEpdDisplay(int) | 0x98cdc | 0x120 | 0x7e5e0 | 34 / 0x22 | 0x7b00c |

The exported wrappers call the indicated virtual slots. BpSurfaceComposer's
vtable maps these to the listed proxy implementations. Each proxy writes the
interface token and one signed 32-bit integer, calls Binder transact with flags
zero, then reads a signed 32-bit reply. BnSurfaceComposer::onTransact at 0x7aab8
indexes a signed-relative jump table at 0xa5e24 by transaction minus one.
The three entries independently resolve to the server cases above, which check
the interface, read one integer, call the corresponding virtual slot, and write
its integer result to the reply (shared tail at 0x7b6d8).

Reproduce disassembly with tools/Inspect-StockElf.py against the private stock
libgui.so, using --symbol 'SurfaceComposerClient.*(Epd|DisplayType)',
--address 0x7e310 --size 0x390, and --symbol 'BnSurfaceComposer10onTransact'.
The tool annotates ordinary AArch64 PLT calls; it does not decode Android packed
relocations or claim to reconstruct all C++ semantics.

These numbers describe this stock build only. They must not be sent to current
SurfaceFlinger, where transaction numbers can mean different operations.
Valid mode values, screen/power/touch sequencing, permission checks in the actual
SurfaceFlinger implementations, and HWC behavior remain unresolved. No calls were
sent to the phone during this analysis.

## Java service recovered from VDEX

The stock files use VDEX 019 with compact DEX. Built anestisb/vdexExtractor
commit 78f283b60ab6991fa27eeaff7d7be16409401c08 locally in WSL. GCC 13 required
`CC=gcc -Wno-error=vla-parameter` for its pre-existing array declaration warning;
other warnings remain errors. Extraction/unquickening completed without ignoring
CRC errors: three framework compact DEX files and one services compact DEX file.
The extractor's --dis output supplies bytecode with resolved method/field names;
no compact-to-standard DEX conversion or Java decompilation has been performed.

Private focused outputs: firmware/extracted/eink-framework-disassembly.txt and
eink-services-disassembly.txt, generated with tools/Filter-VdexDisassembly.py.
Full outputs are under /home/a6l/analysis/vdex in WSL.

Observed in EpdManagerService:

- `onBootPhase(500)` calls SurfaceControl.connectEpdDisplay(1), then
  setExternalEnabled(false). Connection therefore does not imply rear touch is on.
- `setExternalEnabled(boolean)` writes ASCII "1" or "0" to
  /sys/ctp1/ctp_func/tpenable; it is a touch control despite its broad name.
- `getEpdModeString(int)` names 0 mirror, 1 typing, 2 picture, 3 reading,
  6 fast and 8 video. This describes service names, not yet verified waveforms.
- `setEpdModeImmediately(int)` passes its original argument to
  SurfaceControl.setEpdMode; its debug override changes the stored/logged mode
  field but not that call's argument. Do not assume logs alone prove active mode.
- `setDisplayTypeLocked(int)` coordinates notifications, display power,
  delayed handler messages, and mode changes. It is not a single sysfs operation.
- `setSensorTypeLocked(int)` also sets sys.mirror.set and writes to
  /sys/debug_control/mirror/state, a different path from the init permission
  reference /sys/kernel/mirror/state. Resolve live aliases before implementation.

Tool source: https://github.com/anestisb/vdexExtractor

## Appended device trees

tools/Extract-AppendedDtb.py recovered two structurally bounded FDT blobs from
kernel-stock after the gzip stream. Private outputs, SHA-256 values, offsets,
decoded DTS files and dtc warning logs are in firmware/extracted/device-trees.
Both decode successfully; dtc emits warnings for the original vendor tree.
Neither tree nor a device-tree overlay has been selected/modified for a new boot.

In stock-00.dts:

- /soc/spi@c1b8000/eink,ed052tc2@0 has chip select 0 and a 19.2 MHz SPI limit.
- The secondary DSI controller references qcom,mdss_dsi_epd_eink_qhd_video.
  This node specifies a transport of 384 x 725 at 85 Hz with 24 bpp and two DSI
  lanes. These are not the Android e-ink display's logical 720 x 1440 dimensions
  or a claim that the e-ink panel visibly refreshes at 85 Hz.
- Secondary controller GPIO properties include EPD XON, EPD power-on, DSI-to-DPI
  supply enable, reset, and EPD I2C enable. This supports investigating the
  software TCON/bridge path rather than treating the panel as an ordinary DSI LCD.

The spare was unauthorized during the initial tree extraction. Authorization
was subsequently restored and a read-only baseline captured. A later visibility
check finds /sys/kernel/mirror absent and /sys/debug_control/mirror inaccessible;
chosen tree contents also deny shell access. Active tree/overlay selection and
alias resolution remain incomplete. See kernel-investigation-20260914.md for
recovered kernel handler symbols, callback records and uevent behavior.
