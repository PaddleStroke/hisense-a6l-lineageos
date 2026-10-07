#!/vendor/bin/sh
# A6L rom-v1/rom-v2 module loader (agent flash; rom-v2 groups misc/charger + audio fallback by agent merge, 25 Sep 2026). Replaces the bundle run.sh insmod sequences proven from RAM on the phone.
# usage: a6l-modules.sh display|adsp|misc|offcharge|bootinfo|displaywait <gpu|done> <max_s>   (display also loads the base group)
# Lists: /vendor/etc/a6l/modules/<group>.txt, one "file.ko [params...]" per line, '#' comments, loaded in order.
# Never rmmod msm (oops in adreno_remove). Everything is logged to the kernel log (stdio_to_kmsg) with prefix A6L_ROM.
M=/vendor/lib/modules
L=/vendor/etc/a6l/modules
# r6b boot fix: tmpfs hand-off dir (display markers, e-ink mode for a6l_epdd)
A=/dev/a6l
log() { echo "A6L_ROM $*"; }
loaded() { grep -q "^$(echo "$1" | tr - _) " /proc/modules; }
load_list() {
    [ -f "$L/$1.txt" ] || { log "$1: no list"; return 0; }
    fails=0
    while read -r ko params; do
        case "$ko" in ''|'#'*) continue;; esac
        n=${ko%.ko}
        loaded "$n" && continue
        # rom-v2: fall back to the upstream q6routing (the module of the 24 Sep call test) if the per-direction one misbehaves
        [ "$ko" = q6routing.ko ] && [ "$(getprop persist.vendor.a6l.audio.q6routing)" = upstream ] && [ -f "$M/q6routing-upstream.ko" ] && ko=q6routing-upstream.ko
        # Debug ROMs: retain the last 256 VM map/unmap operations for the GPU
        # devcoredump. Set at module load so the first process VM is covered.
        # No rendering, frequency, voltage or recovery behavior changes.
        if [ "$ko" = msm.ko ] && [ "$(getprop ro.build.type)" != user ]; then
            params="$params vm_log_shift=8"
            # Retained V67/r5 modules lack this new diagnostic parameter.
            # Only opt in when the exact module's modinfo declares it.
            if grep -a -q 'parm=fault_bo_log:' "$M/$ko"; then
                params="$params fault_bo_log=1"
            fi
        fi
        # eink-round6b (7 Oct 2026, firmware/extracted/eink-round6-20261007 README part B): fixed DPU planes. Linux 7.2's msm
        # defaults to virtual planes, which move the 5 SSPPs of the SDM660 DPU between the LCD and the rear e-ink CRTC at every
        # plane change. A CTL whose interface stopped keeps the flush bits of the SSPPs it last staged; staged by the other
        # CRTC, that SSPP's flush never completes: LCD "vblank timeout: 1001000/1001/1" after every e-ink lock/minute frame,
        # composer commits EBUSY, setPowerMode 1.2-2.3 s, LCD touch dead, the power-key long-press menu (12:14 live log).
        # With fixed planes each DRM plane keeps its SSPP (LCD: VIG0 + DMA0-2 via the composer, e-ink lease: VIG1).
        # Only when the module declares the parameter (else insmod would fail); a6l.dpu_virtual_planes=1 on the kernel
        # command line keeps the 7.2 default.
        if [ "$ko" = msm.ko ] && grep -a -q 'parmtype=dpu_use_virtual_planes:' "$M/$ko" && \
           ! grep -q 'a6l.dpu_virtual_planes=1' /proc/cmdline; then
            params="$params dpu_use_virtual_planes=0"
        fi
        # A6L hw-ISP (6 Oct 2026, hw-isp-20261006): VFE0 PIX line + hardware Bayer ISP for the main camera (libcamera
        # picks the cameras, persist.vendor.a6l.hwisp.sensors; default since hw-isp round 6: all three cameras when the
        # module has the PIX link-rate clock fix (parameter a6l_pix_linkcap), else imx576 only).
        # Opt out with persist.vendor.a6l.hwisp=0 (RDI + CPU soft ISP for all, as before). Only when the module declares it.
        if [ "$ko" = qcom-camss.ko ] && [ "$(getprop persist.vendor.a6l.hwisp)" != 0 ] && \
           grep -a -q 'parm=a6l_pix:' "$M/$ko"; then
            params="$params a6l_pix=1"
        fi
        # r6b boot fix (30 Sep 2026): log BEFORE each insmod, so the last line of a hung boot names the module that never returned
        log "$1: insmod $ko ..."
        # restart-hang-20261007: show msm.ko init steps (info level) on the boot console, so a stuck boot names its last step
        [ "$ko" = msm.ko ] && { pk=$(cat /proc/sys/kernel/printk); echo 7 > /proc/sys/kernel/printk; }
        if insmod "$M/$ko" $params; then log "$1: insmod $ko ok"; else log "$1: insmod $ko FAILED rc=$?"; fails=$((fails+1)); fi
        [ "$ko" = msm.ko ] && echo "$pk" > /proc/sys/kernel/printk
    done < "$L/$1.txt"
    log "$1: done fails=$fails"
}
# r5 review fix F11 (28 Sep 2026) + r6b boot fix (30 Sep 2026): deterministic, boot-scoped EGL selection, now bounded.
# The display group starts in early-init, before persistent properties exist, so it cannot set persist.graphics.egl itself
# (a value set now is overwritten by load_persist_props, and a vendor domain may not set it in enforcing). It publishes the
# choice of THIS boot in the non-persistent vendor.a6l.gpu, in BOTH branches; init.qcom.rc (vendor_init, allowed to set
# graphics_config_writable_prop) waits for it at post-fs-data (exec_start a6l_display_wait = `displaywait gpu 40`, bounded)
# and copies it into persist.graphics.egl after load_persist_props and before zygote/SurfaceFlinger, so a stale value from an
# earlier boot is always replaced (unset => angle). persist.graphics.egl is the loader's first key
# (frameworks/native/opengl/libs/EGL/Loader.cpp: persist.graphics.egl, ro.hardware.egl, ro.board.platform).
# Bound: mesa as soon as renderD128 exists; otherwise angle 10 s after the lists are loaded (as before), or 30 s after the
# start when they are still loading (an insmod hangs: the GPU may never come; SurfaceFlinger then renders in software).
decide() {
    i=0; j=0
    while [ ! -e /sys/class/drm/renderD128 ] && [ $i -lt 150 ] && [ $j -lt 50 ]; do
        [ -e $A/display.loaded ] && j=$((j+1))
        sleep 0.2; i=$((i+1))
    done
    # r6d (30 Sep 2026, docs/rom-r6d-20260930.md): mesa only when the Mesa set exists for EVERY ABI the zygotes run. The
    # loader takes the first set property and never falls back: r6c had only /vendor/lib64 Mesa, so zygote_secondary
    # (app_process32) aborted in eglGetDisplay and the zygote restart loop never ended. r6d ships /vendor/lib too; if a set
    # is incomplete anyway, angle (system/lib + system/lib64) is chosen instead of an abort.
    miss=""
    for f in /vendor/lib64/egl/libEGL_mesa.so /vendor/lib64/egl/libGLESv2_mesa.so /vendor/lib64/libgallium_dri.so \
             /vendor/lib/egl/libEGL_mesa.so /vendor/lib/egl/libGLESv2_mesa.so /vendor/lib/libgallium_dri.so; do
        case $f in /vendor/lib/*) [ -n "$(getprop ro.product.cpu.abilist32)" ] || continue;; esac
        [ -e $f ] || miss="$miss $f"
    done
    if [ -e /sys/class/drm/renderD128 ] && [ -n "$miss" ]; then
        gpu=angle; log "display: renderD128 after ${i}x0.2s but Mesa incomplete (missing:$miss) -> EGL angle (software)"
    elif [ -e /sys/class/drm/renderD128 ]; then
        # Adreno 512 up: Mesa freedreno (vendor/lib{,64}/egl/*_mesa.so). No hardware Vulkan is claimed (a5xx: no Turnip).
        gpu=mesa; log "display: renderD128 after ${i}x0.2s -> EGL mesa"
    elif [ -e $A/display.loaded ]; then
        # software fallback: libEGL_angle/libGLESv2_angle over the software vulkan.pastel (a6l-software-graphics.mk)
        gpu=angle; log "display: NO renderD128 -> EGL angle (software)"
    else
        gpu=angle; log "display: module lists still loading after ${i}x0.2s (an insmod hangs?) -> EGL angle (software)"
    fi
    setprop vendor.a6l.gpu $gpu || log "display: setprop vendor.a6l.gpu $gpu FAILED (init selects angle)"
}
case "$1" in
display)
    # 1. e-ink PMIC (TPS65185; gpio42/56 are DT fixed regulators since V71)  2. GPU msm separate_gpu_kms=1
    # 3. panels: LCD FT8719 + e-ink panel (V73 panel-a6l-epd-dsi)  4. touch/input
    # r5 bug hunt boot-init: base.txt first = Android-required kernel features built as modules (fuse, uhid)
    # r6b boot fix (30 Sep 2026, docs/rom-r6b-bootfix-20260930.md): init STARTS this group at early-init (`start`, no longer
    # `exec_start`). r6's first boot hung for ever in early-init because an insmod of this group never returned (after the msm
    # probe), so init never reached mount_all, USB or adbd. Now this main process loads the lists and may block without
    # blocking init; the decider child below ALWAYS publishes vendor.a6l.gpu within a bound; vendor.a6l.display tracks the
    # state (loading -> loaded -> done; stays "loading" if an insmod hangs) for the adsp group and charger mode (bounded waits).
    mkdir -p $A; rm -f $A/display.loaded
    setprop vendor.a6l.display loading
    # userdebug/eng (r6b): wait (bounded, 20 s) until the persistent boot log (rom/debug, /metadata/a6l/cur/kmsg.*, started
    # right after mount_all since r6c) runs, so a boot that dies inside a display insmod still leaves its log - with the "insmod X ..." line -
    # on the eMMC. Not in user builds (no boot log) and not in off-mode charging (no mount_all, no boot log).
    if [ "$(getprop ro.build.type)" != user ] && [ "$(getprop ro.bootmode)" != charger ]; then
        i=0; while [ $i -lt 100 ] && [ "$(getprop vendor.a6l.bootlog)" != running ]; do sleep 0.2; i=$((i+1)); done
        log "display: boot log $(getprop vendor.a6l.bootlog) after ${i}x0.2s (debug build: lists load after it)"
    fi
    # restart hang (7 Oct 2026, firmware/extracted/restart-hang-20261007/krec): debug builds run a synchronous kmsg
    # recorder on raw reserve2 from early-init; make sure it is recording before the first insmod (bounded, 5 s)
    if [ "$(getprop ro.vendor.a6l.krec)" = 1 ]; then
        i=0; while [ $i -lt 25 ] && [ ! -e /dev/a6l-krec.ready ]; do sleep 0.2; i=$((i+1)); done
        log "display: kmsg recorder '$(cat /dev/a6l-krec.ready 2>/dev/null)' after ${i}x0.2s"
    fi
    decide &
    dpid=$!
    load_list base
    load_list display
    : > $A/display.loaded; setprop vendor.a6l.display loaded
    wait $dpid
    # wait for the LCD connector (DSI-1, 1080x2340) so the composer finds it at start (r6b: init waits for "done" at
    # post-fs-data, bounded; if this group hangs the composer starts without it and is restarted by init until it appears)
    i=0; while [ $i -lt 75 ]; do
        for c in /sys/class/drm/card*-DSI-*; do [ -e "$c/status" ] && grep -q 1080x2340 "$c/modes" 2>/dev/null && break 2; done
        sleep 0.2; i=$((i+1)); done
    lcd=""; for c in /sys/class/drm/card*-DSI-*; do [ -e "$c/status" ] && grep -q 1080x2340 "$c/modes" 2>/dev/null && lcd=${c##*/}; done
    if [ -z "$lcd" ]; then
        # msm/DSI did not come up: fall back to the bootloader splash framebuffer (a6l_simplefb -> simpledrm) so the UI
        # still shows, as in the V64-V70 phone runs (software rendering).
        if loaded a6l_simplefb; then log "display: LCD connector missing, a6l_simplefb already loaded"
        elif insmod $M/a6l_simplefb.ko; then log "display: LCD connector missing -> a6l_simplefb fallback"
        else log "display: LCD connector missing, simplefb fallback failed"; fi
    else
        log "display: LCD $lcd"
    fi
    # The e-ink (384x725 DSI connector on the same msm card) must NOT become a second SurfaceFlinger display:
    # drm_hwcomposer attaches every connected DSI connector and keeps DRM master (docs/eink-mirror-milestone-20260923.md).
    # Force it off before the composer starts; a6l_epdd_v3 --lease auto takes it back through a DRM lease later.
    for c in /sys/class/drm/card*-DSI-*; do
        [ -e "$c/status" ] || continue
        m=$(head -n 1 "$c/modes" 2>/dev/null)
        log "display: ${c##*/} status=$(cat $c/status) mode=$m"
        # rom-v2: with the eink3 composer patch (ignore_connectors + lease server, vendor.hwc.drm.lease_socket set) the
        # composer skips the e-ink by mode and leases it to a6l_epdd: it must stay CONNECTED (a forced-off connector
        # reports no modes, so the ignore rule would not match and the lease would fail). rom-v1 path otherwise.
        case "$m" in 384x725*) echo "$m" > $A/eink-mode
            if [ -n "$(getprop vendor.hwc.drm.lease_socket)" ]; then log "display: e-ink ${c##*/} left connected (composer lease mode)"
            else echo off > "$c/status"; log "display: e-ink ${c##*/} forced off (mode saved)"; fi;; esac
    done
    setprop vendor.a6l.display done
    ;;
adsp)
    # sensor registry -> ADSP remoteproc -> start -> audio card -> TMD3702 ALS/proximity (all non-RF)
    # r6b boot fix: the display group is no longer finished by `on boot` by construction (non-blocking start). Its lists
    # carry shared providers (mdt_loader, qcom_aoss, llcc-qcom, ...): wait until they are loaded, bounded (30 s), then go on
    # anyway (a hung display insmod must not keep audio/sensors/radio down).
    i=0; while [ $i -lt 30 ] && [ "$(getprop vendor.a6l.display)" = loading ]; do sleep 1; i=$((i+1)); done
    log "adsp: display group $(getprop vendor.a6l.display) after ${i}s"
    load_list adsp
    r=""; for x in /sys/class/remoteproc/remoteproc*; do case "$(cat $x/name 2>/dev/null)" in *15700000*|*adsp*) r=$x;; esac; done
    if [ -n "$r" ]; then
        [ "$(cat $r/state)" = running ] || echo start > $r/state
        i=0; while [ $i -lt 30 ] && [ "$(cat $r/state)" != running ]; do sleep 1; i=$((i+1)); done
        log "adsp: ${r##*/} state=$(cat $r/state) after ${i}s"
    else
        log "adsp: no ADSP remoteproc"
    fi
    sleep 3
    load_list audio
    i=0; while [ $i -lt 20 ] && ! grep -q "Hisense A6L" /proc/asound/cards 2>/dev/null; do sleep 1; i=$((i+1)); done
    log "adsp: sound card $(grep -c 'Hisense A6L' /proc/asound/cards 2>/dev/null) after ${i}s"
    # senshal (26 Sep): SMGR motion IIO (qcom-smgr-accel/gyro/mag) -> uid system for sensors.a6l (the multihal runs as
    # system). ueventd.rc has the same rules; this is the fallback. sensors.a6l retries every 2 s, so late is fine.
    i=0; while [ $i -lt 10 ] && [ "$(cat /sys/bus/iio/devices/iio:device*/name 2>/dev/null | grep -c '^qcom-smgr-')" -lt 3 ]; do sleep 1; i=$((i+1)); done
    for d in /sys/bus/iio/devices/iio:device*; do
        case "$(cat $d/name 2>/dev/null)" in qcom-smgr-*) ;; *) continue;; esac
        for f in $d/buffer/enable $d/buffer/length $d/buffer/watermark $d/scan_elements/*_en; do
            [ -e "$f" ] && chown system:system "$f" && chmod 0664 "$f"
        done
        [ -c "/dev/${d##*/}" ] && chown system:system "/dev/${d##*/}" && chmod 0660 "/dev/${d##*/}"
        log "adsp: sensors ${d##*/} $(cat $d/name) -> system (after ${i}s)"
    done
    # TMD3702 (rear-facing ALS/proximity at 0x49 on i2c c176000), no DT node: instantiate by hand
    if insmod $M/tmd3702.ko; then
        for b in /sys/bus/i2c/devices/i2c-*; do readlink -f "$b" | grep -q c176000 && { echo "tmd3702 0x49" > $b/new_device; log "adsp: tmd3702 on ${b##*/}"; }; done
    fi
    ;;
misc)
    # rom-v2: fuel gauge, vibrator, flash LED, e-ink frontlight, cameras (probe only); charger only when enabled
    load_list misc
    if [ "$(getprop persist.vendor.a6l.charger)" = 1 ]; then load_list charger; else log "misc: charger driver NOT loaded (persist.vendor.a6l.charger != 1; PMIC defaults)"; fi
    # merge2 (25 Sep): camera3 stack is EXPERIMENTAL (probe PASS; IMX576 streams but VFE0 status1 bit29 fires every frame;
    # no camera HAL): loaded only with persist.vendor.a6l.camera=1. Nothing may dump camss/VFE registers of a
    # powered-off block (reset seen at stream-off, 25 Sep).
    if [ "$(getprop persist.vendor.a6l.camera)" = 1 ]; then load_list camera; else log "misc: camera stack NOT loaded (persist.vendor.a6l.camera != 1)"; fi
    for l in /sys/class/leds/epd-backlight /sys/class/leds/*flash* /sys/class/leds/*torch*; do [ -e "$l/brightness" ] && log "misc: led ${l##*/}"; done
    # r5 review fix F49: VibratorOL's LED backend (init restarts vendor.qti.vibrator when this group stops)
    if [ -e /sys/class/leds/vibrator/activate ]; then log "misc: vibrator led ok"; else log "misc: vibrator led MISSING (VibratorOL will find no device)"; fi
    for x in /sys/class/power_supply/*; do [ -e "$x/type" ] && log "misc: power_supply ${x##*/} $(cat $x/type) capacity=$(cat $x/capacity 2>/dev/null)"; done
    ;;
offcharge)
    # H49 (r5, 28 Sep 2026): off-mode charging (init `on charger`: no `on boot`, no /data). Only what the charger needs:
    # the fuel gauge (health HAL / charger UI telemetry, misc.txt's pmi8998_fg) and the charger group (charger.txt) when
    # enabled; no ADSP/audio, radio, camera, vibrator or frontlight. The display group already ran in early-init.
    # NOTE: persistent properties are not loaded in charger mode, so persist.vendor.a6l.charger is its BUILD default here.
    if loaded pmi8998_fg; then :; elif insmod $M/pmi8998_fg.ko; then log "offcharge: insmod pmi8998_fg.ko ok"; else log "offcharge: insmod pmi8998_fg.ko FAILED rc=$?"; fi
    if [ "$(getprop persist.vendor.a6l.charger)" = 1 ]; then load_list charger
    else log "offcharge: charger driver NOT loaded (persist.vendor.a6l.charger != 1): PMIC hardware/bootloader charging defaults"; fi
    for x in /sys/class/power_supply/*; do [ -e "$x/type" ] && log "offcharge: power_supply ${x##*/} $(cat $x/type) capacity=$(cat $x/capacity 2>/dev/null)"; done
    ;;
displaywait)
    # r6b boot fix: bounded wait used by init (exec_start) instead of blocking on the display group itself.
    #   gpu  <s>: until vendor.a6l.gpu is published (post-fs-data, before the persist.graphics.egl copy)
    #   done <s>: until the display group finished (LCD connector wait included; charger mode, before the charger UI)
    n=$(( ${3:-40} * 5 )); i=0
    case "$2" in
    gpu)  while [ $i -lt $n ] && [ -z "$(getprop vendor.a6l.gpu)" ]; do sleep 0.2; i=$((i+1)); done;;
    done) while [ $i -lt $n ] && [ "$(getprop vendor.a6l.display)" != done ]; do sleep 0.2; i=$((i+1)); done;;
    *) log "displaywait: unknown mode $2"; exit 2;;
    esac
    log "displaywait $2: vendor.a6l.gpu='$(getprop vendor.a6l.gpu)' vendor.a6l.display='$(getprop vendor.a6l.display)' after ${i}x0.2s"
    ;;
bootinfo)
    log "bootinfo: kernel=$(uname -r) image=$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-image 2>/dev/null)"
    log "bootinfo: mounts $(grep -E ' /(system|vendor|data|mnt/vendor/persist) ' /proc/mounts | cut -d' ' -f1-3 | tr '\n' ';')"
    log "bootinfo: modules $(wc -l < /proc/modules) drm=$(ls /sys/class/drm | tr '\n' ' ')"
    ;;
*) log "unknown group $1"; exit 2;;
esac
exit 0
