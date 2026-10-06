Superseded during the r6 prep (29 Sep 2026): the android-usb worker delivered USB device mode in parallel
(device/hisense/a6l/usb + rom/init/init.a6l.usb.rc, merged into rom.mk: gadget HAL, UDC genfs, udc overlay flag).
These two files are the pure-init alternative (no gadget HAL, sys.usb.configfs=2 property rules for every
function set) kept only as a fallback if the gadget HAL fails on the phone. Renamed .txt so no build or policy picks
them up. Not referenced by r6.mk or apply-r6.sh.
