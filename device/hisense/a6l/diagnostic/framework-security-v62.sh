#!/system/bin/sh
# Diskless VM only: genuine keystore2 and gatekeeperd. No KeyMint/Gatekeeper HAL,
# TEE or persistent storage exists here (gatekeeperd uses its software fallback);
# this establishes framework Binder peers, not A6L security properties.
set -eu
grep -q virt /proc/device-tree/model || exit 95
echo A6L_SECURITY_BEGIN
mkdir -p /data/misc/keystore /data/misc/gatekeeper
# V63+: standard post-fs-data directories that framework services expect.
mkdir -p /data/misc/adb /data/misc/keychain /data/misc/systemkeys /data/misc/wifi /data/misc/apns /data/misc/carrierid \
    /data/misc/network_watchlist /data/misc/textclassifier /data/misc/perfetto-traces /data/misc/stats-data /data/misc/stats-service \
    /data/system_de/0 /data/system_ce/0 /data/misc_de/0 /data/misc_ce/0 /data/user/0 /data/user_de/0 /data/media/0 \
    /data/system/users/0 /data/system/dropbox /data/system/heapdump /data/backup /data/ota /data/local/tmp /data/anr /data/tombstones
chown -R 1000:1000 /data/system /data/system_de /data/system_ce /data/misc/adb /data/misc/keychain /data/misc/systemkeys
if [ -x /vendor/bin/hw/android.hardware.security.keymint-service.nonsecure ]; then
    timeout --foreground -k 3 1500 /vendor/bin/hw/android.hardware.security.keymint-service.nonsecure > /logs/keymint.log 2>&1 &
    k=0
    while [ "$k" -lt 20 ]; do
        service check android.hardware.security.keymint.IKeyMintDevice/default | grep -q ': found' && { echo A6L_KEYMINT_NONSECURE_SERVICE_PASS; break; }
        sleep 1; k=$((k+1))
    done
    [ "$k" -lt 20 ] || { echo A6L_KEYMINT_SERVICE_MISSING; head -n 10 /logs/keymint.log; }
fi
timeout --foreground -k 3 1500 /system/bin/keystore2 /data/misc/keystore > /logs/keystore2.log 2>&1 &
timeout --foreground -k 3 1500 /system/bin/gatekeeperd /data/misc/gatekeeper > /logs/gatekeeperd.log 2>&1 &
i=0; ks=0; gk=0
while [ "$i" -lt 20 ]; do
    [ "$ks" -eq 1 ] || { service check android.system.keystore2.IKeystoreService/default | grep -q ': found' && { ks=1; echo A6L_KEYSTORE2_SERVICE_PASS; }; } || true
    [ "$gk" -eq 1 ] || { service check android.service.gatekeeper.IGateKeeperService | grep -q ': found' && { gk=1; echo A6L_GATEKEEPERD_SERVICE_PASS; }; } || true
    [ "$ks" -eq 1 ] && [ "$gk" -eq 1 ] && exit 0
    sleep 1
    i=$((i+1))
done
[ "$ks" -eq 1 ] || { echo A6L_KEYSTORE2_SERVICE_PENDING; head -n 20 /logs/keystore2.log; }
[ "$gk" -eq 1 ] || { echo A6L_GATEKEEPERD_SERVICE_PENDING; head -n 20 /logs/gatekeeperd.log; }
# keystore2 may still be initialising; the harness re-checks after SystemServer.
exit 0
