#!/bin/bash
# Offline test of host/diag-bootwrite.sh with a mock `adb` (fake phone: sysfs, 64 MiB boot partition file, /tmp).
# Cases: wrong geometry refused; expect-current mismatch refused (partition untouched); write OK + readback; already-installed;
# dd that writes nothing detected. usage: tests/test-bootwrite-mock.sh   -> BOOTWRITE_MOCK_PASS
set -u; H=$(cd "$(dirname "$0")/../host" && pwd); W=$(mktemp -d); trap 'rm -rf $W' EXIT
mkdir -p $W/bin $W/phone/tmp $W/phone/sys/class/block/mmcblk1p29 $W/phone/dev $W/kit/host $W/kit/logs
cp $H/diag-bootwrite.sh $W/kit/host/
B=$W/phone/bootpart; head -c 67108864 /dev/urandom > $B; OLD=$(sha256sum $B | cut -c1-64)
printf 'MAJOR=179\nMINOR=29\nDEVNAME=mmcblk1p29\nDEVTYPE=partition\nPARTNAME=boot\n' > $W/phone/sys/class/block/mmcblk1p29/uevent
echo 179:29 > $W/phone/sys/class/block/mmcblk1p29/dev; echo 671744 > $W/phone/sys/class/block/mmcblk1p29/start; echo 131072 > $W/phone/sys/class/block/mmcblk1p29/size
cat > $W/bin/adb <<'A'
#!/bin/bash
P=$MOCK/phone
if [ "$1" = devices ]; then echo "List of devices attached"; echo -e "HLTE730T-PROBE\trecovery"; exit 0; fi
[ "$1" = -s ] && shift 2
case $1 in
push) cp "$2" "$P${3}"; echo "$2: 1 file pushed"; exit 0;;
pull) cp "$P${2}" "$3"; exit 0;;
shell) c=$2
  c=${c//\/system\/bin\/toybox /}; c=${c//\/tmp\//$P/tmp/}; c=${c//\/sys\/class\/block/$P/sys/class/block}
  c=${c//\/dev\/block\/a6ldiag/$P/dev/a6ldiag}; c=${c//\/proc\/sys\/vm\/drop_caches//dev/null}; c=${c//\/proc\/modules//dev/null}
  echo "WARNING: linker: mock warning" >&2
  mknod() { ln -sf $MOCK/phone/bootpart "$1"; }; insmod() { :; }
  if [ -n "${MOCK_DD_NOOP:-}" ]; then dd() { case "$*" in *of=*a6ldiag*) echo "dd: unknown conv"; return 1;; *) command dd "$@";; esac; }; fi
  export -f mknod insmod; [ -n "${MOCK_DD_NOOP:-}" ] && export -f dd
  bash -c "$c"; exit 0;;
esac
A
chmod 755 $W/bin/adb; export PATH=$W/bin:$PATH MOCK=$W
IMG=$W/new.img; head -c 67108864 /dev/urandom > $IMG; NEW=$(sha256sum $IMG | cut -c1-64)
fail=0; ck() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; echo "$o" | sed "s/^/    | /"; fail=1; fi; }
# 1 expect-current mismatch
o=$($W/kit/host/diag-bootwrite.sh $IMG --expect-current 0000000000000000000000000000000000000000000000000000000000000000 2>&1)
ck expect_mismatch_refused '[[ "$o" == *"is not the expected"* ]] && [ "$(sha256sum $B | cut -c1-64)" = $OLD ] && ls $W/kit/logs/boot-before-*.img >/dev/null'
# 2 dd writes nothing
o=$(MOCK_DD_NOOP=1 $W/kit/host/diag-bootwrite.sh $IMG 2>&1); ck dd_noop_detected '[[ "$o" == *"dd wrote nothing"* ]] && [ "$(sha256sum $B | cut -c1-64)" = $OLD ]'
# 3 good write
o=$($W/kit/host/diag-bootwrite.sh $IMG --expect-current $OLD 2>&1); ck write_ok '[[ "$o" == *"BOOTWRITE_OK $NEW"* ]] && [ "$(sha256sum $B | cut -c1-64)" = $NEW ] && [ ! -e $W/phone/tmp/boot-new.img ]'
# 4 already
o=$($W/kit/host/diag-bootwrite.sh $IMG 2>&1); ck already '[[ "$o" == *BOOTWRITE_OK_ALREADY* ]]'
# 5 restore from the backup of case 3 (= OLD)
BK=$(for f in $W/kit/logs/boot-before-*.img; do [ "$(sha256sum $f | cut -c1-64)" = $OLD ] && echo $f; done | head -n 1); sleep 1
o=$($W/kit/host/diag-bootwrite.sh $BK --expect-current $NEW 2>&1); ck restore_ok '[[ "$o" == *"BOOTWRITE_OK $OLD"* ]] && [ "$(sha256sum $B | cut -c1-64)" = $OLD ]'
# 6 geometry
echo 671745 > $W/phone/sys/class/block/mmcblk1p29/start
o=$($W/kit/host/diag-bootwrite.sh $IMG 2>&1); ck geometry_refused '[[ "$o" == *"unexpected boot geometry"* ]] && [ "$(sha256sum $B | cut -c1-64)" = $OLD ]'
[ $fail = 0 ] && echo BOOTWRITE_MOCK_PASS || { echo BOOTWRITE_MOCK_FAIL; exit 1; }
