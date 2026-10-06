#!/bin/bash
# volte4 bundle builder (WSL relay). No compilation: downloads the pinned Alpine v3.22 aarch64 packages
# (qmi-utils 1.36.0 = qmicli, libqmi, libqrtr-glib, glib, ... musl), flattens the runtime closure of qmicli,
# adds the stock Orange FR MBN (pinned), the test script, and verifies qmicli under qemu-aarch64.
set -euo pipefail
A=/mnt/c/Users/Pierre/Desktop/A6L; W=~/vo5; OUT=$W/bundle/volte4; REL=v3.22
MCFG=$A/firmware/extracted/peripheral-firmware-20260917-r3/modem/IMAGE/MODEM_PR/MCFG/CONFIGS/MCFG_SW
PKGS="qmi-utils-1.36.0-r0 libqmi-1.36.0-r0 libqrtr-glib-1.2.2-r0 libmbim-1.32.0-r0 glib-2.84.4-r0 musl-1.2.5-r12 pcre2-10.46-r0 libffi-3.4.8-r0 libintl-0.24.1-r0 zlib-1.3.2-r0 libmount-2.41.6-r1 libblkid-2.41.6-r1 libeconf-0.6.3-r0"
mkdir -p $W/apk; rm -rf $W/root $OUT; mkdir -p $W/root $OUT/q/lib $OUT/mbn
: > $W/apk-sums.txt
for p in $PKGS; do
  f=$p.apk; repo=main; case $p in qmi-utils*|libqmi*|libqrtr*|libmbim*) repo=community;; esac
  [ -s $W/apk/$f ] || curl -sf -o $W/apk/$f https://dl-cdn.alpinelinux.org/alpine/$REL/$repo/aarch64/$f
  echo "$(sha256sum $W/apk/$f | cut -c1-64)  $f  https://dl-cdn.alpinelinux.org/alpine/$REL/$repo/aarch64/$f  license=$(tar -xzOf $W/apk/$f .PKGINFO 2>/dev/null | sed -n 's/^license = //p')" >> $W/apk-sums.txt
  tar -xzf $W/apk/$f -C $W/root 2>/dev/null
done
need() { readelf -d "$1" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; }
cp -L $W/root/usr/bin/qmicli $OUT/q/qmicli.bin; todo="$OUT/q/qmicli.bin"; seen=""
while [ -n "$todo" ]; do set -- $todo; f=$1; shift; todo="$*"
  for l in $(need $f); do case " $seen " in *" $l "*) continue;; esac; seen="$seen $l"
    [ $l = libc.musl-aarch64.so.1 ] && continue
    src=$( (ls $W/root/usr/lib/$l $W/root/lib/$l 2>/dev/null || true) | head -1); cp -L $src $OUT/q/lib/$l; todo="$todo $OUT/q/lib/$l"; done; done
cp -L $W/root/lib/ld-musl-aarch64.so.1 $OUT/q/ld-musl-aarch64.so.1
cp -L $W/root/lib/ld-musl-aarch64.so.1 $OUT/q/lib/libc.musl-aarch64.so.1
{ echo "qmicli 1.36.0 runtime, unmodified binaries from Alpine Linux $REL aarch64 (fetched $(date -u +%F))."
  echo "Sources: https://gitlab.alpinelinux.org/alpine/aports (community/libqmi, main/glib, ...); upstream https://gitlab.freedesktop.org/mobile-broadband/libqmi"
  echo "Licenses: qmicli GPL-2.0-or-later; libqmi-glib/libqrtr-glib/libmbim-glib/glib LGPL-2.1-or-later; musl MIT (see license= per package)."
  echo "sha256 of the .apk files:"; cat $W/apk-sums.txt; } > $OUT/q/SOURCES.txt
cp $A/tools/volte4-pdc/volte4-test.sh $A/tools/volte4-pdc/qmicli $OUT/; chmod 755 $OUT/volte4-test.sh $OUT/qmicli $OUT/q/qmicli.bin $OUT/q/ld-musl-aarch64.so.1
cp $MCFG/GENERIC/EU/ORANGE/COMMERCI/FRANCE/MCFG_SW.MBN $OUT/mbn/France-Commercial-Orange.mbn
python3 - $OUT/mbn/France-Commercial-Orange.mbn > $OUT/mbn/MBN-INFO.txt <<'PY'
import sys,struct,hashlib
d=open(sys.argv[1],'rb').read()
ph=struct.unpack_from('<I',d,0x1c)[0]; n=struct.unpack_from('<H',d,0x2c)[0]
i=d.find(b'MCFG'); fmt,=struct.unpack_from('<H',d,i+4); ver,=struct.unpack_from('<I',d,i+0x14)
k=d.find(b'MCFG_TRL'); j=d.find(b'France-Commercial-Orange')
print("source: NON-HLOS MCFG_SW/GENERIC/EU/ORANGE/COMMERCI/FRANCE/MCFG_SW.MBN (stock A6L modem firmware, unmodified)")
print("size:", len(d)); print("sha256:", hashlib.sha256(d).hexdigest()); print("sha1 (= qmicli config id):", hashlib.sha1(d).hexdigest())
print("ELF32 program headers:", n, "; MCFG header at 0x%x format %d; version 0x%08x (trailer MCFG_TRL at 0x%x)"%(i,fmt,ver,k))
print("description in trailer: '%s' at 0x%x"%(d[j:j+24].decode(),j))
PY
( cd $OUT && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS )
cat $OUT/mbn/MBN-INFO.txt
echo "--- qemu checks"
Q="env A6L_QEMU=qemu-aarch64 sh $OUT/qmicli"
$Q --version | head -1
$Q -d qrtr://0 --help-pdc | grep -E "pdc-(list|load|activate|deactivate|delete|noop)"
$Q --help | grep -i "qrtr"
$Q -d qrtr://0 --pdc-list-configs=software 2>&1 | head -3 || true; echo "(expected failure here: WSL kernel has no AF_QIPCRTR)"
du -sh $OUT; ls -la $OUT $OUT/q $OUT/mbn
