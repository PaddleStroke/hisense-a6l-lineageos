#!/vendor/bin/sh
# Debug builds only. Read an existing MSM devcoredump; never read debugfs gpu/rd,
# trigger a reset, clear the dump, or change rendering/power settings.
# One bounded capture per device kind per boot (GPU and display). Full prefixes:
# /data/vendor/a6l-gpu-debug (2 x 8 MiB). Metadata headers: 2 x 128 KiB.
# Keep >= 1.25 MiB metadata free; retain fixed files across boots.
PATH=/vendor/bin:/system/bin
umask 077
DATA=${A6L_GPU_DATA:-/data/vendor/a6l-gpu-debug}
META=${A6L_GPU_META:-/metadata/a6l/gpu-debug}
SYS=${A6L_GPU_SYS:-/sys/class/devcoredump}
KMSG=${A6L_GPU_KMSG:-/dev/kmsg}
MAX=${A6L_GPU_MAX:-8388608}
TICKS=${A6L_GPU_TICKS:-0}
SLEEP=${A6L_GPU_SLEEP:-0.25}
log() { echo "A6L_GPU_COREDUMP $*" > "$KMSG"; }
boot=$(cat /proc/sys/kernel/random/boot_id)
count=0
captured=""
while :; do
    for dump in "$SYS"/devcd*/data; do
        [ -r "$dump" ] || continue
        # MSM display also publishes devcoredumps. Keep it separately so an early
        # display dump cannot prevent capture of the later GPU translation fault.
        device=${dump%/data}/failing_device
        case "$(readlink -f "$device")" in
            *5000000.gpu*) kind=gpu;;
            *c901000.display-controller*) kind=display;;
            *) continue;;
        esac
        case " $captured " in *" $kind "*) continue;; esac
        [ -d /data/vendor ] || [ -n "$A6L_GPU_DATA" ] || continue
        mkdir -p "$DATA" || exit 1
        free=$(df -k "$DATA" | awk 'END {print $4}')
        case "$free" in ''|*[!0-9]*) log "no data free-space reading"; exit 1;; esac
        need=$((MAX / 1024 + 16384))
        [ "$free" -ge "$need" ] || { log "skip data-free-kb=$free need=$need"; exit 1; }
        # Only these fixed files are replaced. Existing stock/ROM backups are untouched.
        log "start boot=$boot kind=$kind source=$dump cap=$MAX"
        if ! timeout 8 head -c "$MAX" "$dump" > "$DATA/$kind.devcore.tmp"; then
            log "read incomplete (bounded timeout); retaining partial prefix"
        fi
        mv -f "$DATA/$kind.devcore.tmp" "$DATA/$kind.devcore"
        { echo "boot=$boot"; echo "kind=$kind"; echo "source=$dump"; echo "cap=$MAX";
          echo "bytes=$(stat -c %s "$DATA/$kind.devcore")";
          echo "uptime=$(cat /proc/uptime)"; } > "$DATA/$kind.info"
        mkdir -p "$META"
        free=$(df -k "$META" | awk 'END {print $4}')
        case "$free" in ''|*[!0-9]*) free=0;; esac
        if [ "$free" -ge 1408 ]; then
            # Header includes fault address, TTBR/page-table walk and VM history
            # before large ASCII85 buffers. Fixed 128 KiB, retained across boots.
            head -c 131072 "$DATA/$kind.devcore" > "$META/$kind.devcore"
            cp "$DATA/$kind.info" "$META/$kind.info"
            sync -f "$META/$kind.devcore"
        else
            log "metadata-prefix skipped free-kb=$free (data copy retained)"
        fi
        sync -f "$DATA/$kind.devcore"
        log "saved boot=$boot kind=$kind bytes=$(stat -c %s "$DATA/$kind.devcore")"
        captured="$captured $kind"
    done
    count=$((count+1))
    [ "$TICKS" -eq 0 ] || [ "$count" -lt "$TICKS" ] || exit 0
    sleep "$SLEEP"
done
