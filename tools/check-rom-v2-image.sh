#!/usr/bin/env bash
# merge2 (25 Sep 2026; r4 items by merge 28 Sep): offline content check of the built rom-v2 system/vendor (after `m systemimage vendorimage`).
# WSL, read-only, < 30 s. Checks the merge2 items + Astra config items in the product out dir.
O=/home/a6l/android/a6l-lineage24/out/target/product/a6l; V=$O/vendor; S=$O/system; bad=0
ok() { echo "ok   $*"; }; no() { echo "FAIL $*"; bad=$((bad+1)); }
for f in lib/modules/stk3338_a6l.ko lib/hw/sensors.a6l.so lib64/hw/sensors.a6l.so lib64/android.hardware.sensors@2.0-subhal-impl-1.0.so \
         etc/a6l/modules/camera.txt etc/a6l/modules/adsp.txt etc/sensors/hals.conf etc/permissions/android.hardware.sensor.light.xml \
         etc/permissions/android.hardware.sensor.proximity.xml lib/modules/ipa2_lite.ko lib/modules/q6routing.ko lib/modules/q6routing-upstream.ko; do
  [ -e $V/$f ] && ok "vendor/$f" || { case $f in lib/hw/*) echo "info vendor/$f absent (64-bit only)";; *) no "vendor/$f missing";; esac; }; done
grep -q "stk3338_a6l.ko ps_thd_high=600 ps_thd_low=300" $V/etc/a6l/modules/adsp.txt && ok "adsp.txt stk hysteresis" || no "adsp.txt stk line"
grep -q "subhal-impl-1.0" $V/etc/sensors/hals.conf && ok "hals.conf has the 1.0 sub-HAL" || no "hals.conf"
grep -q "qcom-camss" $V/etc/a6l/modules/misc.txt && no "camss still in misc.txt" || ok "camera not in misc.txt (gated)"
# r6d: A6L_DEBUG_ADBKEY=1 = the explicit debug variant (Pierre, 30 Sep): the laptop key MUST be there, public part only
if [ "${A6L_DEBUG_ADBKEY:-0}" = 1 ]; then
  K=/mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted/laptop-adb-public-key.txt
  cmp -s $V/etc/a6l/adb_keys $K && ok "DEBUG variant: /vendor/etc/a6l/adb_keys = laptop public key ($(sha256sum < $K | cut -c1-8))" || no "DEBUG variant: adb_keys missing or not the laptop key"
  grep -q "PRIVATE" $V/etc/a6l/adb_keys 2>/dev/null && no "adb_keys contains a private key" || ok "adb_keys has no private key material"
else
  [ -e $V/etc/a6l/adb_keys ] && no "RELEASE ships /vendor/etc/a6l/adb_keys" || ok "no adb key in vendor (release)"
fi
grep -q "property:ro.vendor.a6l.debug.adbkey=1" $V/etc/init/hw/init.qcom.rc && ok "adb key copy gated" || no "init.qcom.rc adb gate"
grep -q "zramsize=50%" $V/etc/fstab.qcom && ok "zram fstab" || no "zram fstab"
P="$V/build.prop $V/etc/build.prop $V/default.prop $S/build.prop $S/system/build.prop $O/product/etc/build.prop $S/product/etc/build.prop"
for k in dalvik.vm.heapgrowthlimit=256m dalvik.vm.heapsize=512m ro.config.low_ram=false ro.lmk.use_psi=true persist.vendor.a6l.camera=1 persist.vendor.a6l.radio=1 ro.hardware.sensors=a6l ro.product.first_api_level=37; do
  cat $P 2>/dev/null | grep -q "^$k$" && ok "prop $k" || no "prop $k ($(cat $P 2>/dev/null | grep "^${k%%=*}=" | head -2 | tr '\n' ' '))"; done
if [ "${A6L_DEBUG_ADBKEY:-0}" = 1 ]; then cat $P 2>/dev/null | grep -q "^ro.vendor.a6l.debug.adbkey=1$" && ok "DEBUG variant: prop ro.vendor.a6l.debug.adbkey=1" || no "DEBUG variant: adbkey prop missing"
else cat $P 2>/dev/null | grep -q "^ro.vendor.a6l.debug.adbkey=" && no "debug adbkey prop in release" || ok "no debug adbkey prop"; fi
# merge3 (r3) items
M=$V/lib/modules; hs() { sha256sum < $1 | cut -c1-8; }
for f in a6l_gpio_vib.ko led-class-multicolor.ko leds-qcom-lpg.ko leds-pwm.ko; do [ -e $M/$f ] && ok "r3 module $f" || no "r3 module $f missing"; done
for f in qcom-spmi-haptics.ko a6l_pm660_haptics.ko; do [ -e $M/$f ] && no "$f shipped (VibratorOL would take the PM660 FF device)" || ok "no $f"; done
grep -q "^a6l_gpio_vib.ko" $V/etc/a6l/modules/misc.txt && ok "misc.txt loads a6l_gpio_vib" || no "misc.txt vib"
# merge r4 (28 Sep): ipa4 + camfix5 replace ipa3 + camfix3
# rom r6 (30 Sep 2026; docs/rom-r6-build-20260929.md): pins accept the v67 OR the r5-kernel build of each merged module
pin() { case " $2 " in *" $(hs $M/$1) "*) ok "$1 = $3 ($(hs $M/$1))";; *) no "$1 sha $(hs $M/$1) (want one of $2: $3)";; esac; }
pin ipa2_lite.ko "910daa9f 28fe7dc3" "bh2k K4 ipa4 (v67|r5)"
pin qcom-camss.ko "4d1dc227 43c5d6ca" "camera-rom1 a6l_wm=3 (v67|r5)"
pin q6adm.ko "$(sha256sum < /mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted/audio5-20260925/modules/q6adm.ko | cut -c1-8) 0c20a33f" "audio5 endpoint_id_2 (v67|r5 rebuild)"
grep -q 'MultiMedia2 Mixer LPI_MI2S_TX_3" value="1"' $V/etc/mixer_paths_a6l.xml && ok "mixer: MM2 capture route on" || no "mixer MM2 route"
sed '/<module name="r_submix"/,$d' $V/etc/audio_policy_configuration.xml | grep -q 'AUDIO_CHANNEL_IN_STEREO' && no "primary module still offers stereo input" || ok "primary module inputs mono (r_submix untouched)"
for k in ro.vendor.a6l.audio.capture_pcm=1 ro.vendor.a6l.ril.dpm_open_port=1 ro.vendor.a6l.ril.slots=2 persist.vendor.a6l.ipa=1 persist.vendor.a6l.charger=1 ro.vendor.a6l.rom.build=${A6L_CHECK_MARKER:-r6e} persist.vendor.a6l.ril.volte=0; do
  cat $P 2>/dev/null | grep -q "^$k$" && ok "prop $k" || no "prop $k"; done
grep -q "xtra3grcej" $V/etc/gps_debug.conf 2>/dev/null && ok "gps_debug.conf xtra3grcej" || echo "info gps_debug.conf: $(ls $V/etc/gps_debug.conf 2>&1 | head -1)"
# merge r4 (28 Sep) items
R=/mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted
pin qcom_smbx.ko "e2f64c51 08dbe9eb" "fastcharge-rom-20260929 hvdcp (v67|r5)"
pin snd-soc-msm8916-analog.ko "167841f5 8b7c1610" "MBHC v2 + bh2k K2 (v67|r5)"
pin btqca.ko "ea345ab1 1aedeebe" "no-MSFT (v67|r5 rebuild)"
grep -q "^qcom_smbx.ko fcc_max_ua=2400000 jeita_hard=1 hvdcp_enable=1 hvdcp_max_uv=9000000 hvdcp_icl_ua=2000000" $V/etc/a6l/modules/charger.txt && ok "charger.txt stock FCC" || no "charger.txt params"
for f in bin/a6l-chg-guard.sh etc/init/init.a6l-power.rc firmware/regulatory.db firmware/regulatory.db.p7s bin/a6l-imsdcm etc/init/a6l-imsdcm.rc; do [ -e $V/$f ] && ok "vendor/$f" || no "vendor/$f missing"; done
grep -q "property:persist.vendor.a6l.radio=1" $V/etc/init/a6l-imsdcm.rc && ok "imsdcm trigger uses persist.vendor.a6l.radio" || no "imsdcm trigger"
grep -q "ORDER RULE" $V/bin/a6l-radio.sh && ok "a6l-radio.sh IPA-before-modem rule" || no "a6l-radio.sh ipa order"
[ "$(sha256sum < $V/firmware/regulatory.db | cut -c1-8)" = 5560f4f0 ] && ok "regulatory.db = attended bytes" || no "regulatory.db sha"
# rom r6 items (30 Sep 2026)
for f in bin/hw/android.hardware.thermal-service.a6l etc/thermal-a6l.conf bin/hw/android.hardware.usb.gadget-service.a6l bin/hw/android.hardware.camera.provider-service.a6l lib64/hw/camera.libcamera.so etc/libcamera/camera_hal.yaml etc/init/init.a6l-watchdog.rc bin/hw/android.hardware.health-service.example etc/res/images/charger/battery_scale.png apex/com.android.hardware.gatekeeper.nonsecure.apex lib/modules/xt_quota2.ko lib/modules/uid_sys_stats.ko lib/modules/qcom-wdt.ko lib/modules/qcom-spmi-adc5.ko lib/modules/qcom-vadc-common.ko; do [ -e $V/$f ] && ok "r6 vendor/$f" || no "r6 vendor/$f missing"; done
for k in xt_quota2.ko uid_sys_stats.ko qcom-wdt.ko; do grep -qx "$k" $V/etc/a6l/modules/base.txt && ok "base.txt loads $k" || no "base.txt $k"; done
grep -q "service vendor.charger .*--charger" $V/etc/init/android.hardware.health-service.example.rc && ok "off-mode charger UI service" || no "charger UI service"
cat $P 2>/dev/null | grep -q "^persist.vendor.a6l.watchdog=1" && no "watchdog on by default" || ok "watchdog opt-in (off by default)"
cat $P 2>/dev/null | grep -q "^persist.vendor.a6l.btcall.bridge=1" && no "btcall bridge on by default" || ok "btcall bridge off by default"
for k in bluetooth.profile.a2dp.source.enabled=true bluetooth.profile.hfp.ag.enabled=true bluetooth.hfp.software_datapath.enabled=true persist.sys.usb.config=adb; do cat $P 2>/dev/null | grep -q "^$k$" && ok "prop $k" || no "prop $k"; done
grep -qs "^lineage.updater.uri=https://updater.invalid/" $S/system_ext/etc/build.prop && ok "updater uri = unpublished placeholder (non-release)" || no "updater uri"
[ -e $S/system_ext/priv-app/A6LDisplaySwitcher ] && ok "A6LDisplaySwitcher (e-ink settings)" || no "A6LDisplaySwitcher missing"
grep -qsi "contrast.af\|^ *af:" $V/share/libcamera/ipa/simple/*.yaml && no "camera AF block shipped (not decided)" || ok "no camera AF block (as decided)"
echo "-- feature XMLs (vendor + system):"; ls $V/etc/permissions $S/etc/permissions 2>/dev/null | grep -E "sensor|vulkan|camera|telephony|nfc|fingerprint" | sort | tr '\n' ' '; echo
# senshal (26 Sep): the compass IS served by sensors.a6l (AK09918 via SMGR, proven 27 Sep) -> compass XML expected; no vulkan
ls $V/etc/permissions 2>/dev/null | grep -q "sensor.compass" && ok "compass feature XML (sensors.a6l mag)" || no "compass feature XML missing"
ls $V/etc/permissions $S/etc/permissions 2>/dev/null | grep -qE "hardware.vulkan" && no "vulkan feature XML" || ok "no hardware vulkan feature XML"
grep -qs 'unavailable-feature name="android.hardware.sensor.compass"' $V/etc/permissions/a6l-unavailable-features.xml && no "compass still withdrawn" || ok "compass not withdrawn"
# lc2 (29 Sep): the libcamera provider ships; the camera IS advertised by a6l-camera-features.xml and must not be withdrawn
grep -qs 'feature name="android.hardware.camera"' $V/etc/permissions/a6l-camera-features.xml && ! grep -qs 'unavailable-feature name="android.hardware.camera"' $V/etc/permissions/a6l-unavailable-features.xml && ok "camera advertised (a6l-camera-features.xml, libcamera provider)" || no "camera feature"
grep -l "android.hardware.vulkan" $V/etc/permissions/*.xml $S/etc/permissions/*.xml 2>/dev/null | head -3
# r6b boot fix (30 Sep 2026, docs/rom-r6b-bootfix-20260930.md): display group non-blocking, bounded waits, debug boot log
Q=$V/etc/init/hw/init.qcom.rc
grep -v '^ *#' $Q | grep -q 'exec_start a6l_modules_display' && no "init.qcom.rc still blocks on the display group" || ok "display group not exec_start'ed"
awk '/^on early-init$/{f=1;next} /^(on|service) /{f=0} f' $Q | grep -q '^ *start a6l_modules_display$' && ok "early-init starts the display group" || no "early-init start a6l_modules_display"
grep -q '^service a6l_display_wait /vendor/bin/a6l-modules.sh displaywait done 45$' $Q && ok "bounded a6l_display_wait (45 s)" || no "a6l_display_wait service"
grep -q 'insmod $ko \.\.\."' $V/bin/a6l-modules.sh && grep -q '^displaywait)' $V/bin/a6l-modules.sh && ok "a6l-modules.sh r6b (insmod pre-log, displaywait)" || no "a6l-modules.sh not r6b"
[ "$(grep -c 'write /dev/kmsg "A6L_STAGE' $Q)" = 6 ] && ok "A6L_STAGE markers" || no "A6L_STAGE markers"
if cat $P 2>/dev/null | grep -q '^ro.build.type=user$'; then
  [ -e $V/etc/init/init.a6l.bootlog-debug.rc ] || [ -e $V/bin/a6l-bootlog.sh ] && no "user build ships the debug boot log" || ok "user build: no debug boot log"
else
  [ -e $V/etc/init/init.a6l.bootlog-debug.rc ] && [ -e $V/bin/a6l-bootlog.sh ] && ok "userdebug: /metadata boot log service" || no "userdebug boot log missing"
fi
# r6c (30 Sep 2026, docs/rom-r6c-20260930.md): msm minigbm, USB HAL DAC + adopt, logcat boot log, format without discard
for f in bin/hw/android.hardware.graphics.allocator-service.minigbm lib64/libminigbm_gralloc.so lib64/hw/mapper.minigbm.so; do [ -e $V/$f ] && ok "r6c vendor/$f" || no "r6c vendor/$f missing"; done
strings $V/lib64/libminigbm_gralloc.so 2>/dev/null | grep -q 'DRM_IOCTL_MSM_GEM_NEW failed' && ok "libminigbm_gralloc has the msm backend (DRV_MSM)" || no "libminigbm_gralloc without the msm backend"
strings $V/lib64/libminigbm_gralloc.so 2>/dev/null | grep -qx 'simpledrm' && ok "libminigbm_gralloc keeps the simpledrm backend (QEMU / LCD fallback)" || no "simpledrm backend lost"
cat $P 2>/dev/null | grep -q '^vendor.minigbm.debug=nocompression$' && ok "prop vendor.minigbm.debug=nocompression (linear)" || no "prop vendor.minigbm.debug"
cat $P 2>/dev/null | grep -q '^ro.hardware.gralloc=minigbm$' && ok "prop ro.hardware.gralloc=minigbm (mapper.minigbm)" || no "prop ro.hardware.gralloc"
U=$V/etc/init/hw/init.a6l.usb.rc
for a in UDC idVendor idProduct bDeviceClass configs/b.1/strings/0x409/configuration; do grep -q "^ *chown system system /config/usb_gadget/g1/$a$" $U && ok "usb rc: $a owned by the HAL user" || no "usb rc chown $a"; done
grep -q '^ *chown system system /sys/class/udc/a800000.usb/soft_connect$' $U && ok "usb rc: soft_connect owned by the HAL user" || no "usb rc soft_connect chown"
strings $V/bin/hw/android.hardware.usb.gadget-service.a6l | grep -q 'handing it back to init' && strings $V/bin/hw/android.hardware.usb.gadget-service.a6l | grep -q 'adopted' && ok "gadget HAL r6c (adopt + hand back)" || no "gadget HAL not r6c"
grep -q 'gadget_bound' $V/bin/a6l-chg-guard.sh && ok "charge guard Q6 (soft_connect only on a bound gadget)" || no "charge guard Q6"
[ -e $V/etc/mke2fs.a6l.conf ] && grep -q '^ *discard = false$' $V/etc/mke2fs.a6l.conf && ok "vendor mke2fs profile (discard = false)" || no "mke2fs.a6l.conf"
awk '/^on fs$/{f=1;next} /^(on|service) /{f=0} f' $Q | grep -v '^ *#' | grep -q 'export MKE2FS_CONFIG /vendor/etc/mke2fs.a6l.conf' && ok "mount_all under the no-discard profile" || no "MKE2FS_CONFIG export"
if ! cat $P 2>/dev/null | grep -q '^ro.build.type=user$'; then
  grep -q 'LOGCAT -b $LCB -v threadtime $LCF' $V/bin/a6l-bootlog.sh && grep -q '^CAP_KB=${A6L_BL_CAP_KB:-3072}' $V/bin/a6l-bootlog.sh && grep -q 'KH=2; KT=2; LH=2; LT=2' $V/bin/a6l-bootlog.sh && grep -q '^tombs()' $V/bin/a6l-bootlog.sh && ok "boot log r6d (filtered logcat, head 2, tombstones, budget)" || no "boot log not r6d"
  awk '/^on fs$/{f=1;next} /^(on|service) /{f=0} f' $V/etc/init/init.a6l.bootlog-debug.rc | grep -q 'start a6l_bootlog' && ok "boot log starts on fs (after mount_all)" || no "boot log start trigger"
fi
# r6d (30 Sep 2026, docs/rom-r6d-20260930.md): 32-bit Mesa (zygote_secondary), EGL guard, charge guard Q7
for f in lib/egl/libEGL_mesa.so lib/egl/libGLESv1_CM_mesa.so lib/egl/libGLESv2_mesa.so lib/libgallium_dri.so; do
  [ -e $V/$f ] && readelf -h $V/$f | grep -q 'Class:.*ELF32' && readelf -h $V/$f | grep -q 'Machine:.*ARM$' && ok "r6d vendor/$f (ELF32 ARM)" || no "r6d vendor/$f missing or not ELF32 ARM"; done
for f in lib64/egl/libEGL_mesa.so lib64/egl/libGLESv1_CM_mesa.so lib64/egl/libGLESv2_mesa.so lib64/libgallium_dri.so; do
  [ -e $V/$f ] && readelf -h $V/$f | grep -q 'Class:.*ELF64' && ok "vendor/$f (ELF64)" || no "vendor/$f missing or not ELF64"; done
M32=/mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted/mesa-arm32-r6d-20260930
( cd $V && sha256sum -c --quiet $M32/SHA256SUMS ) && ok "r6d 32-bit Mesa = pinned build (mesa-arm32-r6d SHA256SUMS)" || no "r6d 32-bit Mesa differs from the pinned build"
cat $P 2>/dev/null | grep -q '^ro.zygote=zygote64_32$' && ok "ro.zygote=zygote64_32 (32-bit zygote kept, needs lib/egl)" || echo "info ro.zygote is $(cat $P 2>/dev/null | grep '^ro.zygote=' | tail -1)"
# ro.product.cpu.abilist32 itself is derived by init at runtime from ro.{vendor,system,...}.product.cpu.abilist32 (r6c props: set)
cat $P 2>/dev/null | grep -q -E '^ro.(system|vendor).product.cpu.abilist32=armeabi-v7a' && ok "abilist32 set (init derives ro.product.cpu.abilist32; EGL guard checks lib/egl)" || no "abilist32 not armeabi-v7a"
grep -q 'Mesa incomplete (missing:' $V/bin/a6l-modules.sh && ok "a6l-modules.sh: mesa only with every ABI's Mesa (else angle)" || no "a6l-modules.sh EGL guard"
grep -q '^host_seen()' $V/bin/a6l-chg-guard.sh && grep -q 'Q7: unplugged/unknown never disconnects' $V/bin/a6l-chg-guard.sh && ok "charge guard Q7 (disconnect only on a confirmed DCP without a host)" || no "charge guard Q7"
for f in system/lib/libEGL_angle.so system/lib64/libEGL_angle.so; do [ -e $O/$f ] && ok "$f (angle fallback both ABIs)" || no "$f missing"; done
# r6e (1 Oct 2026, docs/rom-r6e-20261001.md): no online discard, radio binaries executable IN THE IMAGE, debug IO-stall detector
grep -v '^ *#' $V/etc/fstab.qcom | grep -q discard && no "r6e vendor fstab.qcom still has discard" || ok "r6e vendor fstab.qcom: no discard mount option"
XV=$(mktemp -d); /home/a6l/android/a6l-lineage24/out/host/linux-x86/bin/fsck.erofs --extract=$XV --no-preserve-owner $O/vendor.img > /dev/null 2>&1
for f in rmtfs tqftpserv diag-router qrtr-lookup iw; do
  [ -x $XV/a6l/radio/bin/$f ] && ok "r6e vendor.img a6l/radio/bin/$f mode $(stat -c %a $XV/a6l/radio/bin/$f)" || no "r6e vendor.img a6l/radio/bin/$f not executable ($(stat -c %a $XV/a6l/radio/bin/$f 2>/dev/null))"; done
[ -x $XV/bin/a6l-radio.sh ] && ok "vendor.img bin/a6l-radio.sh executable" || no "a6l-radio.sh not executable in the image"
if ! cat $P 2>/dev/null | grep -q '^ro.build.type=user$'; then
  [ -x $XV/bin/a6l-iowatch.sh ] && grep -q 'A6L_IOSTALL' $XV/bin/a6l-iowatch.sh && ok "r6e vendor.img bin/a6l-iowatch.sh (executable)" || no "r6e a6l-iowatch.sh missing/not executable in the image"
  R6=$V/etc/init/init.a6l.bootlog-debug.rc
  awk '/^on post-fs-data$/{f=1;next} /^(on|service) /{f=0} f' $R6 | grep -c -E '^ *(write /proc/sys/kernel/dmesg_restrict 0|chmod 0644 /dev/kmsg|start a6l_iowatch)$' | grep -qx 3 && ok "r6e debug rc: dmesg_restrict 0 + /dev/kmsg 0644 + a6l_iowatch at post-fs-data" || no "r6e debug rc"
  grep -q '^ro.logd.kernel=true$' $V/build.prop && ok "r6e ro.logd.kernel=true (vendor)" || no "r6e ro.logd.kernel"
fi
rm -rf $XV
grep -o 'target-level="[0-9]*"' $V/etc/vintf/manifest.xml | head -1
ls -la $O/system.img $O/vendor.img 2>/dev/null
echo "CHECK_ROM_V2_IMAGE $([ $bad = 0 ] && echo PASS || echo "FAIL ($bad)")"
