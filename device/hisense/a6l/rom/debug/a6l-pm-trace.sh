#!/system/bin/sh
# Debug-only owned power trace, installed with the persistent boot logger.
# Lifecycle: start/snapshot/stop /metadata/a6l/cur. No global tracing controls,
# trace_pipe, device register reads, PM-policy or clock/voltage changes.

ACTION=$1
OUT=$2
OUT_ROOT=${A6L_PM_OUT_ROOT:-/metadata/a6l}
TRACE_ROOT=${A6L_PM_TRACE_ROOT:-}
PROC_ROOT=${A6L_PM_PROC_ROOT:-/proc}
NAME=a6l_pm
PROBE=a6l_pm/gadget_resume
DEF='r:a6l_pm/gadget_resume dwc3_gadget_resume ret=$retval:s32'
CAP=131072

case "$ACTION" in start|snapshot|stop) ;; *) exit 2;; esac
[ "$OUT" = "$OUT_ROOT/cur" ] && [ -d "$OUT" ] || exit 2
STATE=$OUT/.pmtrace-owned
STATUS=$OUT/pmtrace-status.txt
INFO=$OUT/pmtrace-info.txt
SNAP=$OUT/pmtrace.txt
TEMP=$OUT/.pmtrace.tmp
STATE_TEMP=$OUT/.pmtrace-state.tmp
status() { printf '%s\n' "$*" > "$STATUS"; }

BOOT=$(cat "$PROC_ROOT/sys/kernel/random/boot_id" 2>/dev/null)
case "$BOOT" in ''|*[!a-fA-F0-9-]*) status 'skip invalid boot identity'; exit 0;; esac
if [ -z "$TRACE_ROOT" ]; then
    if [ -d /sys/kernel/tracing/instances ]; then TRACE_ROOT=/sys/kernel/tracing
    elif [ -d /sys/kernel/debug/tracing/instances ]; then TRACE_ROOT=/sys/kernel/debug/tracing
    else status 'skip tracefs unavailable'; exit 0
    fi
fi
INSTANCE=$TRACE_ROOT/instances/$NAME
EVENTS=$TRACE_ROOT/kprobe_events

inode() { stat -c %i "$INSTANCE" 2>/dev/null; }
write_state() {
    printf '%s\n%s\n%s\n%s\n' "$BOOT" "$INSTANCE" "$OWN_INODE" "$OWN_PROBE" > "$STATE_TEMP" &&
        mv -f "$STATE_TEMP" "$STATE"
}
read_state() {
    [ -f "$STATE" ] || return 1
    { IFS= read -r saved_boot; IFS= read -r saved_path;
      IFS= read -r OWN_INODE; IFS= read -r OWN_PROBE; } < "$STATE"
    [ "$saved_boot" = "$BOOT" ] && [ "$saved_path" = "$INSTANCE" ] || return 1
    case "$OWN_INODE" in ''|*[!0-9]*) return 1;; esac
    case "$OWN_PROBE" in 0|1) ;; *) return 1;; esac
}
owned_instance() { read_state && [ -d "$INSTANCE" ] && [ "$(inode)" = "$OWN_INODE" ]; }
probe_exists() { [ -f "$EVENTS" ] && grep -Eq '^[pr][0-9]*:a6l_pm/gadget_resume[[:space:]]' "$EVENTS"; }
probe_matches() {
    # register_kretprobe fills maxactive when omitted, and trace_kprobe_show
    # serializes it as r<digits>:. Argument text is retained verbatim.
    [ -f "$EVENTS" ] && grep -Eq '^r[0-9]*:a6l_pm/gadget_resume dwc3_gadget_resume ret=\$retval:s32$' "$EVENTS"
}
wait_file() {
    # tracefs creates files synchronously. Bounded wait also covers host mocks.
    n=0
    while [ ! -f "$1" ] && [ "$n" -lt 5 ]; do sleep 0.1; n=$((n + 1)); done
    [ -f "$1" ]
}
cleanup() {
    read_state || { status 'skip cleanup ownership not established'; return 0; }
    failed=0
    if [ -d "$INSTANCE" ]; then
        [ "$(inode)" = "$OWN_INODE" ] || { status 'refuse cleanup instance replaced'; return 0; }
        if [ -f "$INSTANCE/tracing_on" ]; then
            printf '0\n' > "$INSTANCE/tracing_on" 2>/dev/null || failed=1
        fi
        # This is only our newly created instance, never global events/enable.
        if [ -f "$INSTANCE/events/enable" ]; then
            printf '0\n' > "$INSTANCE/events/enable" 2>/dev/null || failed=1
        fi
        for event in power/suspend_resume power/device_pm_callback_start power/device_pm_callback_end "$PROBE"; do
            [ -f "$INSTANCE/events/$event/enable" ] || continue
            printf '0\n' > "$INSTANCE/events/$event/enable" 2>/dev/null || failed=1
        done
    fi
    if [ "$OWN_PROBE" = 1 ]; then
        if probe_matches; then
            # Kernel refuses deletion if another instance still uses this probe.
            printf '%s\n' "-:$PROBE" >> "$EVENTS" 2>/dev/null || failed=1
        elif probe_exists; then
            status 'refuse cleanup probe definition changed'; return 0
        fi
    fi
    if [ -d "$INSTANCE" ]; then rmdir "$INSTANCE" 2>/dev/null || failed=1; fi
    if [ "$failed" = 0 ]; then rm -f "$STATE" "$TEMP" "$STATE_TEMP"; status 'stopped owned tracing cleaned'
    else status 'cleanup incomplete owned state retained'; fi
}

# The replacement can coexist with the retained 128 KiB snapshot. Fail closed
# if the logger's current byte/free-space budget cannot reserve that extra file.
snapshot_headroom() {
    max_kb=${A6L_PM_CAP_KB:-3072}; min_free=${A6L_PM_FREE_KB:-1024}
    for value in "$max_kb" "$min_free"; do
        case "$value" in ''|*[!0-9]*) status 'snapshot skipped invalid budget'; return 1;; esac
        [ "$value" -le 2147483000 ] 2>/dev/null || { status 'snapshot skipped invalid budget'; return 1; }
    done
    used=$(du -sk "$OUT_ROOT" 2>/dev/null | cut -f1)
    free=$(df -k "$OUT" 2>/dev/null | tail -n 1 | awk '{print $4}')
    for value in "$used" "$free"; do
        case "$value" in ''|*[!0-9]*) status 'snapshot skipped budget unavailable'; return 1;; esac
    done
    limit=$(expr "$max_kb" - 128); needed=$(expr "$min_free" + 128)
    if [ "$used" -gt "$limit" ] || [ "$free" -lt "$needed" ]; then
        status "snapshot skipped headroom used_kb=$used free_kb=$free"
        return 1
    fi
}
save_snapshot() {
    owned_instance || { status 'skip snapshot ownership not established'; return 0; }
    snapshot_headroom || return 0
    # Device-complete callbacks alone exceed128KiB: a plain tail discarded the
    # actual idle entry and USB resume probe. Reserve32KiB for sparse phases and
    # probe returns, then96KiB for callback detail, keeping the same disk budget.
    if [ -f "$INSTANCE/trace" ] && {
        printf '# PM phases and gadget return events (tail32KiB)\n'
        grep -E ': (suspend_resume|gadget_resume|dwc3_gadget_ep_cmd):' "$INSTANCE/trace" | tail -c 32768
        printf '\n# Device callback detail (tail96KiB; phases may repeat)\n'
        tail -c 98100 "$INSTANCE/trace"
    } > "$TEMP" 2>/dev/null; then
        mv -f "$TEMP" "$SNAP" && status 'snapshot saved cap=131072'
    else rm -f "$TEMP"; status 'snapshot failed'; fi
}

case "$ACTION" in
start)
    if owned_instance; then status 'already started owned instance'; exit 0; fi
    if [ -e "$INSTANCE" ] || [ -e "$STATE" ]; then status 'skip instance or ownership record already exists'; exit 0; fi
    if ! mkdir "$INSTANCE" 2>/dev/null; then status 'skip instance creation failed'; exit 0; fi
    OWN_INODE=$(inode); OWN_PROBE=0
    case "$OWN_INODE" in ''|*[!0-9]*) status 'skip cannot identify created instance'; rmdir "$INSTANCE" 2>/dev/null; exit 0;; esac
    write_state || { rmdir "$INSTANCE" 2>/dev/null; exit 0; }
    if ! wait_file "$INSTANCE/tracing_on" ||
       ! printf '0\n' > "$INSTANCE/tracing_on" 2>/dev/null ||
       ! printf '256\n' > "$INSTANCE/buffer_size_kb" 2>/dev/null; then
        status 'skip instance setup failed'; cleanup; exit 0
    fi
    # Setting a clock in this instance never changes the global trace clock.
    clock=default
    if [ -f "$INSTANCE/trace_clock" ] && grep -qw boot "$INSTANCE/trace_clock"; then
        if printf 'boot\n' > "$INSTANCE/trace_clock" 2>/dev/null; then clock=boot; fi
    fi
    enabled=0
    printf 'boot=%s\ninstance=%s\ninode=%s\nclock=%s\nper_cpu_kb=256\nsnapshot_cap=%s\n' \
        "$BOOT" "$INSTANCE" "$OWN_INODE" "$clock" "$CAP" > "$INFO"
    for event in power/suspend_resume power/device_pm_callback_start power/device_pm_callback_end; do
        if [ -f "$INSTANCE/events/$event/enable" ] &&
           printf '1\n' > "$INSTANCE/events/$event/enable" 2>/dev/null; then
            enabled=$((enabled + 1)); printf 'event=%s\n' "$event" >> "$INFO"
        else printf 'unavailable=%s\n' "$event" >> "$INFO"; fi
    done
    # EP0 failed to restart on physical resume while gadget_resume returned0.
    # Record the already-issued command and its result, restricted to control
    # endpoints in this owned instance; no extra USB register reads or writes.
    event=dwc3/dwc3_gadget_ep_cmd
    if [ -f "$INSTANCE/events/$event/filter" ] &&
       printf '%s\n' 'name == "ep0out" || name == "ep0in"' > "$INSTANCE/events/$event/filter" 2>/dev/null &&
       printf '1\n' > "$INSTANCE/events/$event/enable" 2>/dev/null; then
        enabled=$((enabled + 1)); printf 'event=%s filter=ep0out-or-ep0in\n' "$event" >> "$INFO"
    else printf 'unavailable=%s-filter-or-enable\n' "$event" >> "$INFO"; fi
    if [ -f "$EVENTS" ] && ! probe_exists &&
       grep -Eq '[[:space:]]dwc3_gadget_resume$' "$PROC_ROOT/kallsyms" 2>/dev/null; then
        if printf '%s\n' "$DEF" >> "$EVENTS" 2>/dev/null; then
            OWN_PROBE=1
            if ! write_state; then
                # This invocation just created the disabled probe. If recording
                # ownership fails, remove it now rather than enable an orphan.
                if probe_matches; then printf '%s\n' "-:$PROBE" >> "$EVENTS" 2>/dev/null; fi
                cleanup; status 'skip cannot record new probe ownership'; exit 0
            fi
            if wait_file "$INSTANCE/events/$PROBE/enable" &&
               printf '1\n' > "$INSTANCE/events/$PROBE/enable" 2>/dev/null; then
                enabled=$((enabled + 1)); printf 'event=%s\n' "$PROBE" >> "$INFO"
            else printf 'unavailable=%s-enable\n' "$PROBE" >> "$INFO"; fi
        else printf 'unavailable=kretprobe-create\n' >> "$INFO"; fi
    else printf 'unavailable=kretprobe-symbol-or-owned-by-other\n' >> "$INFO"; fi
    if [ "$enabled" = 0 ] || ! printf '1\n' > "$INSTANCE/tracing_on" 2>/dev/null; then
        cleanup; status 'skip no usable events or tracing enable failed'; exit 0
    fi
    status "started events=$enabled"
    ;;
snapshot)
    save_snapshot
    ;;
stop)
    # Use the same budget guard for the final replacement, then clean tracing
    # even when insufficient headroom prevented an updated disk snapshot.
    save_snapshot
    cleanup
    ;;
esac
