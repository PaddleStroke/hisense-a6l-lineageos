# V26 upstream search — 2026-09-16

Search focus: SDM660/SDCC5 crash at CMD0/controller dispatch. No confirmed matching fix found.

- Original 2018 report: https://lkml.rescloud.iu.edu/hypermail/linux/kernel/1809.3/01603.html — reports an SDM660 crash on a power-control write.
- Qualcomm follow-up: https://lists.openwall.net/linux-kernel/2018/10/09/159 — proposes checking DT address/interrupt mapping against downstream.
- Reporter response: https://lists.openwall.net/linux-kernel/2018/10/09/165 — says the actual reg field and power IRQ are correct; wrong node name alone does not explain it. This thread does NOT provide a confirmed resolution.
- Local sdm630.dtsi inherited by SDM660 uses controller0xc0c4000, CQHCI0xc0c5000, ICE0xc0c8000; hc_irq SPI110, pwr_irq SPI112; v5 compatible. No obvious mapping omission matching that suggestion. Actual packaged DT should be checked before any hardware change.
- Voltage sequencing discussion: https://lists.openwall.net/linux-kernel/2018/07/20/339 — Qualcomm platform regulator control addresses timing-related CRC/command timeouts. Modern driver already has platform regulator control; not evidence to revert the 2025 regulator-load correction.

V26 first capture was truncated by host overall330second deadline after a long menu wait, before new CMD0 events. First14events show two SDIO-reset IRQ sequences status0x18000 (error+commandtimeout), both handler paths return. This does not prove a CMD0 hardware fault or match the old power-write report. Fix host deadline and repeat unchanged installed firmware first.
