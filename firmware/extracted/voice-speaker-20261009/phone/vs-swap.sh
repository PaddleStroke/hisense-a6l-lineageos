#!/system/bin/sh
# voice-speaker-20261009: live swap for the in-call speakerphone fix. Root shell, NO call active.
#   sh vs-swap.sh daemon   bind-mount the patched a6l-q6voiced over /vendor/bin (route fallback); no card unbind
#   sh vs-swap.sh param    reload the ROM snd-soc-tfa98xx.ko with pcm_no_constraint=1 (no new kernel binary)
#   sh vs-swap.sh tfa      load the patched snd-soc-tfa98xx.ko (0002)
#   sh vs-swap.sh q6v      patched TFA + patched q6cvp/q6voice (0003, volume experiment; q6voice cvd_mode=0 as the ROM)
#   sh vs-swap.sh status
# param/tfa/q6v stop the audio services, unbind the sound card, swap modules, rebind, restart the services (~5 s, no
# audio meanwhile). Undo everything: reboot (nothing on /vendor is modified).
D=${D:-/data/local/tmp/vs}
M=/vendor/lib/modules
DRV=/sys/bus/platform/drivers/snd-sm8250
SVC_STOP="vendor.a6l-q6voiced audioserver vendor.audio-hal-aidl vendor.a6l-audio-route"
SVC_START="vendor.a6l-audio-route vendor.audio-hal-aidl audioserver vendor.a6l-q6voiced"
log() { echo "VS_SWAP $*"; echo "VS_SWAP $*" > /dev/kmsg 2>/dev/null; }
die() { log "ABORT: $*"; exit 1; }

status() {
	for m in snd_soc_tfa98xx q6cvp q6voice q6voice_dai; do
		[ -d /sys/module/$m ] && log "$m srcversion $(cat /sys/module/$m/srcversion 2>/dev/null) refcnt $(cat /sys/module/$m/refcnt 2>/dev/null)" \
			|| log "$m NOT LOADED"
	done
	log "tfa pcm_no_constraint=$(cat /sys/module/snd_soc_tfa98xx/parameters/pcm_no_constraint 2>/dev/null)" \
	    "q6voice cvd_mode=$(cat /sys/module/q6voice/parameters/cvd_mode 2>/dev/null)" \
	    "rx_vol_mode=$(cat /sys/module/q6voice/parameters/rx_vol_mode 2>/dev/null)"
	p=$(pidof a6l-q6voiced)
	log "a6l-q6voiced pid=${p:-none} exe=$( [ -n "$p" ] && sha256sum /proc/$p/exe | cut -c1-16)"
	grep -q ' /vendor/bin/a6l-q6voiced ' /proc/mounts && log "a6l-q6voiced: patched binary bind-mounted" || log "a6l-q6voiced: ROM binary"
	cat /proc/asound/cards
}

find_card() {	# the sdm660 card's platform device name
	for c in /sys/class/sound/card*; do
		[ "$(basename "$(readlink -f $c/device/driver)")" = snd-sm8250 ] && { basename "$(readlink -f $c/device)"; return 0; }
	done
	return 1
}

holders() {
	for p in /proc/[0-9]*; do
		ls -l $p/fd 2>/dev/null | grep -q ' /dev/snd/' && echo "${p#/proc/}:$(cat $p/comm 2>/dev/null)"
	done
}

services_start() { for s in $SVC_START; do start $s; done; }

insmod_or() {	# insmod <new> [params] || reload the ROM module (audio must come back)
	ko=$1; shift
	if insmod "$ko" "$@"; then log "insmod $ko $* ok"; return 0; fi
	log "insmod $ko FAILED, loading the ROM module"; insmod "$M/$(basename "$ko")" "$@"; return 1
}

swap_modules() {
	mode=$1
	DEV=$(find_card) || die "sdm660 sound card not found"
	log "mode $mode card device $DEV"
	for s in $SVC_STOP; do stop $s; done
	sleep 2
	h=$(holders)
	[ -n "$h" ] && { log "/dev/snd still open by: $h"; services_start; die "card busy (unbind would block)"; }
	echo "$DEV" > $DRV/unbind || { services_start; die "unbind failed"; }
	log "card unbound"
	rmmod snd_soc_tfa98xx || log "rmmod snd_soc_tfa98xx failed (refcnt $(cat /sys/module/snd_soc_tfa98xx/refcnt))"
	case $mode in
	param) insmod_or $M/snd-soc-tfa98xx.ko pcm_no_constraint=1 ;;
	tfa)   insmod_or $D/snd-soc-tfa98xx.ko ;;
	q6v)   insmod_or $D/snd-soc-tfa98xx.ko
	       rmmod q6voice_dai && rmmod q6voice && rmmod q6cvp || log "q6voice stack rmmod failed"
	       insmod_or $D/q6cvp.ko
	       insmod_or $D/q6voice.ko cvd_mode=0
	       insmod $M/q6voice-dai.ko && log "insmod ROM q6voice-dai ok" ;;
	esac
	i=0
	until echo "$DEV" > $DRV/bind 2>/dev/null; do
		i=$((i + 1)); [ $i -ge 30 ] && { log "bind still failing after 15 s"; break; }
		sleep 0.5
	done
	sleep 1
	grep -q . /proc/asound/cards && log "cards: $(cat /proc/asound/cards | head -1)"
	services_start
	log "services restarted"
	status
}

[ "$(id -u)" = 0 ] || die "needs root (adb root)"
case "$1" in
status) status; exit 0 ;;
daemon|param|tfa|q6v) ;;
*) echo "usage: sh vs-swap.sh daemon|param|tfa|q6v|status"; exit 2 ;;
esac
[ "$(getprop vendor.a6l.voice.active)" = 1 ] && die "a call is active: hang up first"
case "$1" in
daemon)
	[ -f $D/a6l-q6voiced ] || die "push a6l-q6voiced to $D first"
	chmod 755 $D/a6l-q6voiced
	chcon u:object_r:a6l_q6voiced_exec:s0 $D/a6l-q6voiced || log "chcon failed (permissive: ok)"
	stop vendor.a6l-q6voiced
	grep -q ' /vendor/bin/a6l-q6voiced ' /proc/mounts || mount -o bind $D/a6l-q6voiced /vendor/bin/a6l-q6voiced || die "bind mount failed"
	start vendor.a6l-q6voiced
	sleep 1
	status
	[ "$(sha256sum /proc/$(pidof a6l-q6voiced)/exe | cut -d' ' -f1)" = "$(sha256sum $D/a6l-q6voiced | cut -d' ' -f1)" ] \
		&& log "running the patched daemon" || log "WARNING: the running daemon is NOT the patched binary"
	;;
*)
	for f in snd-soc-tfa98xx.ko q6cvp.ko q6voice.ko; do
		case "$1:$f" in param:*|tfa:q6*) continue ;; esac
		[ -f $D/$f ] || die "push $f to $D first"
	done
	swap_modules "$1"
	;;
esac
