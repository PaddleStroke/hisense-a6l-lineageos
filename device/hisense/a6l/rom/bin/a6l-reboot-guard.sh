#!/vendor/bin/sh
# A6L reboot guard (fix A, firmware/extracted/reboot-hang-20261006/README.md). Started by init at the beginning of the
# shutdown sequence (service a6l_reboot_guard is "shutdown critical"). Runs from /vendor (never unmounted), holds no
# file on /data or /metadata, writes only to /dev/kmsg, /proc/sysrq-trigger and /proc/sys/kernel.
# Properties (optional): persist.vendor.a6l.rebootguard.khang (s in reboot(2), default 15, 0 = off; without fix B the
#                        APSS WDT armed by watchdogd bites 30 s after init SIGTERMs watchdogd, just before reboot(2))
#                        persist.vendor.a6l.rebootguard.uhang (s before reboot(2), default 0 = rely on init's monitor)
trap '' HUP INT TERM PIPE
log() { echo "<3>a6l_reboot_guard: $*" > /dev/kmsg 2>/dev/null; }
now() { read a b < /proc/uptime; echo "${a%.*}"; }
nr() { s=$(cat /proc/1/syscall 2>/dev/null); echo "${s%% *}"; }
khang=$(getprop persist.vendor.a6l.rebootguard.khang); khang=${khang:-15}
uhang=$(getprop persist.vendor.a6l.rebootguard.uhang); uhang=${uhang:-0}
t0=$(now); tsys=; cmd=$(getprop sys.powerctl)
log "armed: '$cmd' mode=$(cat /sys/kernel/reboot/mode) panic=$(cat /proc/sys/kernel/panic) khang=$khang uhang=$uhang"

trip() {
	log "HANG ($1) '$cmd' after $(( $(now) - t0 )) s; init syscall=$(nr)"
	log "init stack: $(tr '\n' '|' < /proc/1/stack 2>/dev/null)"
	echo w > /proc/sysrq-trigger
	sleep 2
	log "init stack: $(tr '\n' '|' < /proc/1/stack 2>/dev/null) -> sysrq b"
	sleep 1
	echo b > /proc/sysrq-trigger
	exit 0
}

while :; do
	sleep 1
	t=$(now)
	if [ -z "$tsys" ]; then
		if [ "$(nr)" = 142 ]; then
			tsys=$t
			log "init in reboot(2) after $((t - t0)) s"
		elif [ "$uhang" -gt 0 ] && [ $((t - t0)) -ge "$uhang" ]; then
			trip userspace
		fi
	elif [ "$khang" -gt 0 ] && [ $((t - tsys)) -ge "$khang" ]; then
		trip kernel
	fi
done
