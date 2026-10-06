#!/usr/bin/env bash
# usb-speed-check.sh (28 Sep 2026) - is the A6L USB-C port wired for SuperSpeed (5 Gbit/s) or USB 2.0 only (480 Mbit/s)?
# Read-only. Run on the laptop with the phone in STOCK Android (adb on), connected by a USB 3 capable USB-C cable
# (many phone cables are USB 2.0 only: if the result is 480M, retry with a cable known to do USB 3, e.g. from an SSD/dock).
# Answer: 5000M = SuperSpeed lanes wired (video over USB-C is at least possible); 480M with a USB 3 cable = USB 2.0 only.
S=${1:-1e529013}
echo "== phone side (UDC current speed)"
adb -s "$S" shell 'for u in /sys/class/udc/*; do echo "$(basename $u): current_speed=$(cat $u/current_speed 2>/dev/null) maximum_speed=$(cat $u/maximum_speed 2>/dev/null)"; done' 2>&1 | grep -v linker
echo "== laptop side (lsusb tree, phone line)"
lsusb -t | grep -B3 -iE "18d1|05c6|mtp|adb|Vendor Specific|Imaging|Still" | head -20
for d in /sys/bus/usb/devices/*; do [ -f $d/idVendor ] || continue; v=$(cat $d/idVendor); case $v in 18d1|05c6|109b) echo "$(basename $d) vid=$v pid=$(cat $d/idProduct) speed=$(cat $d/speed)M product=$(cat $d/product 2>/dev/null)";; esac; done
echo "== verdict"
sp=$(for d in /sys/bus/usb/devices/*; do [ -f $d/idVendor ] && case $(cat $d/idVendor) in 18d1|05c6|109b) cat $d/speed;; esac; done | head -1)
case "$sp" in 5000|10000) echo "A6L_USB_SPEED SUPERSPEED ${sp}M (SS lanes wired)";; 480) echo "A6L_USB_SPEED HIGHSPEED 480M (USB 2.0 link: retry with a known USB 3 cable before concluding)";; *) echo "A6L_USB_SPEED UNKNOWN (phone not found on the laptop bus)";; esac
