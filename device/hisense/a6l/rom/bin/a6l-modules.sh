#!/vendor/bin/sh
# A6L rom-v1 module loader (agent flash). Replaces the bundle run.sh insmod sequences proven from RAM on the phone.
# usage: a6l-modules.sh display|audio|sensors|bootinfo
# Lists: /vendor/etc/a6l/modules/<group>.txt, one "file.ko [params...]" per line, '#' comments, loaded in order.
# Never rmmod msm (oops in adreno_remove). Everything is logged to the kernel log (stdio_to_kmsg) with prefix A6L_ROM.
M=/vendor/lib/modules
L=/vendor/etc/a6l/modules
log() { echo "A6L_ROM $*"; }
loaded() { grep -q "^$(echo "$1" | tr - _) " /proc/modules; }
load_list() {
    [ -f "$L/$1.txt" ] || { log "$1: no list"; return 0; }
    fails=0
    while read -r ko params; do
        case "$ko" in ''|'#'*) continue;; esac
        n=${ko%.ko}
        loaded "$n" && continue
        if insmod "$M/$ko" $params; then log "$1: insmod $ko ok"; else log "$1: insmod $ko FAILED rc=$?"; fails=$((fails+1)); fi
    done < "$L/$1.txt"
    log "$1: done fails=$fails"
}
case "$1" in
display)
    # 1. e-ink PMIC (TPS65185; gpio42/56 are DT fixed regulators since V71)  2. GPU msm separate_gpu_kms=1
    # 3. panels: LCD FT8719 + e-ink panel (V73 panel-a6l-epd-dsi)  4. touch/input
    load_list display
    i=0; while [ $i -lt 50 ] && [ ! -e /sys/class/drm/renderD128 ]; do sleep 0.2; i=$((i+1)); done
    if [ -e /sys/class/drm/renderD128 ]; then
        # Adreno 512 up: Mesa freedreno (vendor/lib64/egl/*_mesa.so). ro.hardware.egl stays 'angle' (software fallback,
        # also what the QEMU test uses); persist.graphics.egl takes precedence in the EGL loader.
        setprop persist.graphics.egl mesa
        log "display: renderD128 after ${i}x0.2s -> EGL mesa"
    else
        log "display: NO renderD128 -> EGL angle (software)"
    fi
    # wait for the LCD connector (DSI-1, 1080x2340) so the composer finds it at start
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
        case "$m" in 384x725*) mkdir -p /dev/a6l; echo "$m" > /dev/a6l/eink-mode; echo off > "$c/status"; log "display: e-ink ${c##*/} forced off (mode saved)";; esac
    done
    ;;
adsp)
    # sensor registry -> ADSP remoteproc -> start -> audio card -> TMD3702 ALS/proximity (all non-RF)
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
    # TMD3702 (rear-facing ALS/proximity at 0x49 on i2c c176000), no DT node: instantiate by hand
    if insmod $M/tmd3702.ko; then
        for b in /sys/bus/i2c/devices/i2c-*; do readlink -f "$b" | grep -q c176000 && { echo "tmd3702 0x49" > $b/new_device; log "adsp: tmd3702 on ${b##*/}"; }; done
    fi
    ;;
bootinfo)
    log "bootinfo: kernel=$(uname -r) image=$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-image 2>/dev/null)"
    log "bootinfo: mounts $(grep -E ' /(system|vendor|data|mnt/vendor/persist) ' /proc/mounts | cut -d' ' -f1-3 | tr '\n' ';')"
    log "bootinfo: modules $(wc -l < /proc/modules) drm=$(ls /sys/class/drm | tr '\n' ' ')"
    ;;
*) log "unknown group $1"; exit 2;;
esac
exit 0
