#!/system/bin/sh
# Diskless VM only: genuine audioserver without any audio HAL. Confirms that
# AudioFlinger/AudioPolicy Binder endpoints exist so AudioService can start;
# says nothing about A6L codec, amplifier, ADSP or routing.
set -eu
/system/bin/a6l-guard.sh || exit 90
echo A6L_MEDIA_BEGIN
mkdir -p /data/misc/audioserver /data/misc/audio /data/misc/media
# V61+: AOSP example AIDL audio HAL from the vendor APEX (no sound hardware behind it).
hal_dir=/apex/com.android.hardware.audio/bin/hw
if [ -x "$hal_dir/android.hardware.audio.service-aidl.example" ]; then
    timeout --foreground -k 3 1500 "$hal_dir/android.hardware.audio.service-aidl.example" > /logs/audio-hal.log 2>&1 &
    timeout --foreground -k 3 1500 "$hal_dir/android.hardware.audio.effect.service-aidl.example" > /logs/audio-effect-hal.log 2>&1 &
    k=0
    while [ "$k" -lt 30 ]; do
        service check android.hardware.audio.core.IModule/default | grep -q ': found' && { echo A6L_AUDIO_HAL_SERVICE_PASS; break; }
        sleep 1; k=$((k+1))
    done
    if [ "$k" -ge 30 ]; then
        echo A6L_AUDIO_HAL_SERVICE_MISSING
        for d in /proc/[0-9]*; do
            case "$(cat $d/comm 2>/dev/null || true)" in android.hardwar*)
                echo "A6L_HAL_PROC $d $(cat $d/cmdline)"
                for t in $d/task/*; do echo "  thread $(cat $t/comm) wchan=$(cat $t/wchan)"; done;;
            esac
        done
        service list | grep -i audio || true
        grep -a "AHAL\|audio" /logs/android.log | tail -n 25 || true
    fi
    service check android.hardware.audio.effect.IFactory/default | grep -q ': found' && echo A6L_AUDIO_EFFECT_HAL_SERVICE_PASS || echo A6L_AUDIO_EFFECT_HAL_MISSING
else
    echo A6L_AUDIO_HAL_APEX_ABSENT
fi
timeout --foreground -k 3 1500 /system/bin/audioserver > /logs/audioserver.log 2>&1 &
audio_pid=$!
# audioserver only publishes media.audio_flinger after AudioPolicy initialises, which
# waits for the framework's 'activity' service: do not block here. The harness checks
# registration after SystemServer has run (A6L_POST_SERVICE lines).
sleep 3
kill -0 "$audio_pid" || { echo A6L_AUDIOSERVER_EXITED; exit 91; }
echo A6L_AUDIOSERVER_STARTED
exit 0
