#!/usr/bin/env bash
# A6L USB (android-usb, 29 Sep 2026): offline wiring check - the gadget HAL, init rc, overlay and sepolicy reach the
# rom-v2 product the pipeline builds (lineage_gsi_a6l.mk -> rom/rom.mk -> usb/usb.mk; BoardConfig.mk -> rom/BoardConfig-rom.mk;
# tools/rom-v2-pipeline.sh syncs usb/). usage: bash device/hisense/a6l/usb/tests/check-usb-wiring.sh [<repo>]
R=${1:-$(cd "$(dirname "$0")/../../../../.." && pwd)}; D=$R/device/hisense/a6l; U=$D/usb; fails=0; n=0
c() { n=$((n+1)); if eval "$2" > /dev/null 2>&1; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }
nocr() { tr -d '\r' < "$1"; }
RC=$(nocr $D/rom/init/init.a6l.usb.rc | grep -v '^ *#')
c "product: lineage_gsi_a6l.mk inherits rom/rom.mk" "nocr $D/lineage_gsi_a6l.mk | grep -q '^\$(call inherit-product, device/hisense/a6l/rom/rom.mk)'"
c "product: rom.mk inherits usb/usb.mk" "nocr $D/rom/rom.mk | grep -q '^\$(call inherit-product, device/hisense/a6l/usb/usb.mk)'"
c "product: rom.mk installs init.a6l.usb.rc" "nocr $D/rom/rom.mk | grep -q 'init/init.a6l.usb.rc:\$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.a6l.usb.rc'"
c "product: init.qcom.rc imports it" "nocr $D/rom/init/init.qcom.rc | grep -q '^import /vendor/etc/init/hw/init.a6l.usb.rc'"
c "usb.mk: namespace" "nocr $U/usb.mk | grep -q '^PRODUCT_SOONG_NAMESPACES += device/hisense/a6l/usb$'"
c "usb.mk: PRODUCT_PACKAGES gadget HAL" "nocr $U/usb.mk | grep -A1 '^PRODUCT_PACKAGES' | grep -q 'android.hardware.usb.gadget-service.a6l'"
c "usb.mk: overlay dir exists" "nocr $U/usb.mk | grep -q 'DEVICE_PACKAGE_OVERLAYS += device/hisense/a6l/usb/overlay' && test -f $U/overlay/frameworks/base/core/res/res/values/config.xml"
c "overlay: UDC sysfs state update on" "grep -q '<bool name=\"config_enableUdcSysfsUsbStateUpdate\">true</bool>' $U/overlay/frameworks/base/core/res/res/values/config.xml"
c "Android.bp: module + namespace" "nocr $U/Android.bp | grep -q 'name: \"android.hardware.usb.gadget-service.a6l\"' && nocr $U/Android.bp | grep -q '^soong_namespace'"
for f in $(nocr $U/Android.bp | grep -o '"gadget/[^"]*"' | tr -d '"'); do c "Android.bp: $f exists" "test -f $U/$f"; done
c "VINTF: IUsbGadget V2 default" "nocr $U/gadget/android.hardware.usb.gadget-service.a6l.xml | tr -d ' \n' | grep -q '<name>android.hardware.usb.gadget</name><version>2</version><interface><name>IUsbGadget</name><instance>default</instance>'"
c "Android.bp: links gadget-V2-ndk" "nocr $U/Android.bp | grep -q 'android.hardware.usb.gadget-V2-ndk'"
c "HAL rc: binary path" "nocr $U/gadget/android.hardware.usb.gadget-service.a6l.rc | grep -q '^service .* /vendor/bin/hw/android.hardware.usb.gadget-service.a6l$'"
c "sepolicy: file_contexts labels the binary" "nocr $U/sepolicy/vendor/file_contexts | grep -q 'android\\\\.hardware\\\\.usb\\\\.gadget-service\\\\.a6l *u:object_r:hal_usb_gadget_default_exec:s0'"
c "sepolicy: dir in the default A6L_SEPOLICY_DIRS" "nocr $D/rom/BoardConfig-rom.mk | grep -q '^A6L_SEPOLICY_DIRS ?=.* usb/sepolicy/vendor'"
c "BoardConfig.mk includes BoardConfig-rom.mk" "nocr $D/BoardConfig.mk | grep -q 'include device/hisense/a6l/rom/BoardConfig-rom.mk'"
P=$R/tools/rom-v2-pipeline.sh
c "pipeline: syncs usb/" "nocr $P | grep -Eq '^  for d in .* usb( [a-z]+)*; do rm -rf [$]T/[$]d; cp -r [$]D/[$]d [$]T/[$]d; done'"
c "pipeline: CRLF-normalises usb/" "nocr $P | grep -Eq 'crlf .*[$]T/usb( |;)'"
c "pipeline: hals/usb-gadget stub still excluded" "nocr $P | grep -q '^  rm -rf \$T/hals/usb-gadget'"
c "product: no second IUsbGadget provider" "! nocr $D/rom/rom.mk $D/lineage_gsi_a6l.mk $D/a6l-software-graphics.mk | grep -v '^ *#' | grep -q 'usb.gadget-service\\.\\(example\\|qti\\)\\|hals/hals.mk'"
c "rc: sys.usb.configfs=2 (system configfs rules idle)" "echo \"\$RC\" | grep -q 'setprop sys.usb.configfs 2' && ! echo \"\$RC\" | grep -q 'sys.usb.configfs 1'"
c "rc: no Android-kernel-only functions" "! echo \"\$RC\" | grep -q 'mtp.gs0\\|ptp.gs1\\|accessory.gs2\\|audio_source.gs3'"
for m in adb mtp ptp; do c "rc: functionfs $m mounted" "echo \"\$RC\" | grep -q '^ *mount functionfs $m /dev/usb-ffs/$m '"; done
for fn in $(grep -o '"[a-z]*\.[a-z0-9]*"' $U/gadget/a6l_gadget_core.cpp | tr -d '"' | grep -E '^(ffs|rndis|ncm|midi)\.' | sort -u); do
  c "rc: creates functions/$fn used by the HAL" "echo \"\$RC\" | grep -q 'mkdir /config/usb_gadget/g1/functions/$fn\$'"; done
c "rc: controller a800000.usb" "echo \"\$RC\" | grep -q 'setprop sys.usb.controller a800000.usb'"
c "rc: early-adb rules gated by vendor.a6l.usb.hal=0" "[ \$(echo \"\$RC\" | grep '^on property:.*sys.usb.config=adb' | grep -vc 'vendor.a6l.usb.hal=0') = 0 ]"
c "rc: early-adb bind releases the a6l_manual_usb hold" "echo \"\$RC\" | grep -q 'soft_connect connect'"
c "HAL: sets the takeover property" "grep -q 'SetProperty(\"vendor.a6l.usb.hal\", \"1\")' $U/gadget/UsbGadget.cpp"
# r6c (30 Sep 2026): the HAL is uid system; every file it writes must be chowned by init (configfs attrs are root 0644,
# soft_connect root 0200) - r6b's HAL failed its first write with EACCES and nothing was ever bound
for a in UDC idVendor idProduct bDeviceClass bDeviceSubClass bDeviceProtocol configs/b.1/strings/0x409/configuration; do
  c "rc: HAL-written $a chowned to system" "echo \"\$RC\" | grep -q '^ *chown system system /config/usb_gadget/g1/$a\$'"; done
c "rc: soft_connect chowned to system (HAL releases the a6l_manual_usb hold)" "echo \"\$RC\" | grep -q '^ *chown system system /sys/class/udc/a800000.usb/soft_connect\$'"
c "HAL rc: user system (why the chowns are needed)" "nocr $U/gadget/android.hardware.usb.gadget-service.a6l.rc | grep -q '^ *user system\$'"
c "HAL: hands the gadget back to init on kApplyFailed" "grep -q 'SetProperty(\"vendor.a6l.usb.hal\", \"0\")' $U/gadget/UsbGadget.cpp"
c "HAL: adopts an identical early binding" "grep -q 'mGadget->matches(comp)' $U/gadget/a6l_gadget_core.cpp"
c "rc: serial number with a fallback" "echo \"\$RC\" | grep -q 'serialnumber \${ro.serialno:-'"
echo "$([ $fails = 0 ] && echo A6L_USB_WIRING PASS || echo A6L_USB_WIRING FAIL) $((n-fails))/$n"; exit $((fails>0))
