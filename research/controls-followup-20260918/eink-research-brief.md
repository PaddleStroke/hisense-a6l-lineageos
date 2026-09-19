# E-ink research brief — 2026-09-18

The three focused searches did not find a primary source documenting the A6L’s exact ED052TC2 waveform image, VCOM calibration format, SPI-flash command map, or a drop-in TCON bridge. The offline notes therefore remain the controlling evidence: the stock device tree names an ED052TC2 SPI device, while the eMMC backup does not contain the separate waveform/calibration storage.

The closest primary hardware reference is TI/Bitland’s **E-INK Control Board 1.0** schematic ([PDF](https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/196/eink-control-board_2D00_20190619.pdf)). Its connector pages explicitly separate `SPI_CS#_S`, `SPI_SI_S`, `SPI_SCK_S`, and `SPI_SO_S` as an “E-ink SPI Flash” level-shift path, while the same design routes VCOM through a TPS65185 PMIC. This supports treating waveform storage and VCOM generation/calibration as separate hardware questions; it does not establish that the A6L uses this exact topology or pinout.

An open hardware investigation of the **ED052TC4** ([discussion](https://github.com/vroland/epdiy/discussions/398)) reports that all panel SPI pins on its 50-pin connector were unconnected and says the flash purpose was unclear. This is relevant as a warning against assuming that an ED052-family panel’s exposed SPI pins provide readable waveform storage. It is a different panel revision and is not evidence of physical equivalence.

A Zynq controller project ([repository](https://github.com/Hanley-Yao/Zynq7010_eink_controller)) reports ED052TC4 display tests using an external TPS65185 and FPGA controller, but gives no compatible A6L bridge, waveform dump, or VCOM-storage protocol. It is only a lead for external-controller architecture.

Unknowns: whether A6L’s “/dev/epd_flash” reaches a board SPI flash, whether the stock driver performs reads through that node, the flash JEDEC ID/address map, and whether VCOM is stored digitally or only set by PMIC registers.

Highest-value next offline investigation: statically trace the stock kernel’s `/dev/epd_flash` file-operations path and SPI-controller chip-select/GPIO binding, then map every read/ioctl caller and address range without issuing writes. This can distinguish real waveform storage from a placeholder interface before any hardware probing.
