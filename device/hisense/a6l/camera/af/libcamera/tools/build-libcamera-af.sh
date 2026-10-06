#!/bin/bash
# Hisense A6L camera phase 3 (AF): libcamera v0.7.2 + ROM patches 0001-0008 (device/hisense/a6l/camera/libcamera/patches,
# NOT modified here) + AF patches 0101-0104 (this directory). Separate dirs in /home/a6l/libcamera-work, never the
# lc2/h846/HAL dirs of the phase 2 workers, never the Lineage tree.
# Usage: build-libcamera-af.sh [step...]   steps: gen src rec bundle hal   (default: src rec bundle)
#   gen   : regenerate patches/0101-0104 from tools/a6l_af_edit.py (+ ../src) on a fresh src-af-gen tree (branch a6l-af)
#   src   : src-af = v0.7.2 + ../../libcamera/patches/*.patch + patches/01*.patch (git branch a6l-af, one commit each)
#   rec   : recovery build (cam utility) prefix /tmp/libcamaf, build-af / stage-af (lc2 options, own dirs/logs)
#   bundle: bundle-af = the lc2 bundle layout + AF tuning yaml + run-af.sh + a6l_afsharp/a6l_afotp (aarch64 static)
#   hal   : Android HAL compile check with the AF patches (build-hal-af / stage-hal-af; nothing packaged into the ROM)
set -euo pipefail
W=/home/a6l/libcamera-work
A=/mnt/c/Users/Pierre/Desktop/A6L/device/hisense/a6l/camera
R=$A/libcamera            # phase 2 (read only here)
F=$A/af                   # this work
S=$W/src-af
NDK=/home/a6l/ndk/android-ndk-r27c; TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
export PATH=$W/venv/bin:$PATH
cd $W
commit() { git -C "$1" add -A && git -C "$1" -c user.name=a6l -c user.email=a6l@local commit -q -m "$2"; }
steps=${*:-src rec bundle}
for s in $steps; do case $s in
gen)
  if [ -f "$R/patches/0016-a6l-focus-exposure-capture-completion.patch" ]; then
    echo "AF_GEN_OBSOLETE: canonical patch 0016 already includes AF 0101-0104; do not regenerate them on that base." >&2
    exit 1
  fi
  G=$W/src-af-gen; rm -rf $G; git clone -q libcamera-src $G; git -C $G checkout -q v0.7.2
  git -C $G apply --whitespace=nowarn $R/patches/*.patch; commit $G "A6L ROM patches 0001-0008"; git -C $G checkout -q -b a6l-af
  python3 $F/libcamera/tools/a6l_af_edit.py $G stats; commit $G "a6l 0101 software_isp: swstats sharpness (focus measure)"
  python3 $F/libcamera/tools/a6l_af_edit.py $G lens;  commit $G "a6l 0102 simple: soft IPA focus lens plumbing"
  python3 $F/libcamera/tools/a6l_af_edit.py $G af;    commit $G "a6l 0103 ipa: simple: contrast AF algorithm (GT9769)"
  python3 $F/libcamera/tools/a6l_af_edit.py $G hal;   commit $G "a6l 0104 android: AF modes, trigger, state and focus distance"
  mkdir -p $F/libcamera/patches
  git -C $G format-patch -q --no-signature --start-number 101 -o $F/libcamera/patches HEAD~4
  ls $F/libcamera/patches; echo AF_GEN_OK
  ;;
src)
  rm -rf $S; git clone -q libcamera-src $S; git -C $S checkout -q v0.7.2
  git -C $S apply --whitespace=nowarn $R/patches/*.patch; commit $S "A6L ROM patches 0001-0008"; git -C $S checkout -q -b a6l-af
  if [ -f "$R/patches/0016-a6l-focus-exposure-capture-completion.patch" ]; then
    echo "AF_INCLUDED_IN_CANONICAL_0016 (legacy 0101-0104 not applied twice)"
  else
    for p in $F/libcamera/patches/01*.patch; do git -C $S am -q --whitespace=nowarn $p; done
  fi
  echo "AF_SRC_OK $(git -C $S describe --tags) $(git -C $S log --oneline v0.7.2..HEAD | wc -l) commits"
  ;;
rec)
  # same meson options as the lc2 recovery build (build-libcamera-a6l.sh libcamera step), own dirs/logs; the shared
  # cross file is only read
  rm -rf build-af stage-af
  meson setup build-af src-af --cross-file $W/a6l-android-aarch64.cross \
    --prefix=/tmp/libcamaf --libdir=lib --buildtype=release -Db_ndebug=false --force-fallback-for=yaml-0.1 \
    -Dpipelines=simple -Dipas=simple -Dsoftisp-gpu=disabled \
    -Dcam=enabled -Dcam-output-kms=disabled -Dcam-output-sdl2=disabled -Dcam-jpeg=disabled \
    -Dandroid=disabled -Dgstreamer=disabled -Dqcam=disabled -Dlc-compliance=disabled -Dpycamera=disabled \
    -Dv4l2=false -Dudev=disabled -Dtracing=disabled -Dlibdw=disabled -Dlibunwind=disabled \
    -Dapps-output-dng=disabled -Ddocumentation=disabled -Dtest=false > $W/af-rec-setup.log 2>&1 \
    || { tail -30 $W/af-rec-setup.log; echo AF_REC_SETUP_FAIL; exit 1; }
  ninja -C build-af -j${J:-4} > $W/af-rec.log 2>&1 || { grep -E -B2 -A10 "error|FAILED" $W/af-rec.log | head -80; echo AF_REC_FAIL; exit 1; }
  echo "rec warnings (non-subproject): $(grep 'warning:' $W/af-rec.log | grep -vc subprojects || true)"
  grep 'warning:' $W/af-rec.log | grep -v subprojects | head -10 || true
  DESTDIR=$W/stage-af meson install -C build-af --no-rebuild >/dev/null
  echo AF_REC_OK
  ;;
bundle)
  B=$W/bundle-af; rm -rf $B; mkdir -p $B; cp -a stage-af/tmp/libcamaf/. $B/
  for f in $(find $B -type f \( -name '*.so*' -o -path '*/bin/*' -o -path '*/libexec/*' \)); do
    file $f | grep -q ELF && $TC/llvm-strip --strip-unneeded $f 2>/dev/null || true; done
  for m in $(find $B/lib/libcamera -name 'ipa_*.so'); do src-af/src/ipa/ipa-sign.sh build-af/src/ipa-priv-key.pem $m $m.sign; done
  cp $R/data/*.yaml $B/share/libcamera/ipa/simple/
  cp $F/libcamera/data/imx576_a6l.yaml $B/share/libcamera/ipa/simple/imx576_a6l.yaml
  rm -rf $B/include $B/lib/pkgconfig $B/lib/*.a $B/bin/libcamera-bug-report
  openssl pkey -in build-af/src/ipa-priv-key.pem -pubout -out $W/ipa-af-pub.pem 2>/dev/null
  for m in $(find $B/lib/libcamera -name 'ipa_*.so'); do
    openssl dgst -sha256 -verify $W/ipa-af-pub.pem -signature $m.sign $m >/dev/null && echo "IPA_SIGN_OK $(basename $m)" \
      || { echo "IPA_SIGN_BAD $m"; exit 1; }; done
  cp $R/run-libcam.sh $F/run-af.sh $F/af-script-auto.yaml $F/af-script-continuous.yaml $B/
  for t in a6l_afsharp a6l_afotp; do
    $TC/aarch64-linux-android30-clang -O2 -Wall -Wextra -Werror -static -o $B/bin/$t $F/tools/$t.c
    $TC/llvm-strip $B/bin/$t
  done
  (cd $B && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS)
  echo "files $(grep -c . $B/SHA256SUMS) SHA256SUMS $(sha256sum $B/SHA256SUMS | cut -c1-16)"; echo AF_BUNDLE_OK
  ;;
hal)
  # Android HAL compile check (same options as the ROM HAL build), own dirs/logs; the shared hal cross file is only read
  [ -f $W/a6l-android-hal.cross ] || { echo "no $W/a6l-android-hal.cross"; exit 1; }
  rm -rf build-hal-af
  meson setup build-hal-af src-af --cross-file $W/a6l-android-hal.cross \
    --prefix=/vendor --libdir=lib64 --sysconfdir=etc --datadir=share --libexecdir=libexec --buildtype=release -Db_ndebug=false \
    --force-fallback-for=yaml-0.1 -Dpipelines=simple -Dipas=simple -Dsoftisp-gpu=disabled \
    -Dcam=disabled -Dandroid=enabled -Dandroid_platform=generic \
    -Dgstreamer=disabled -Dqcam=disabled -Dlc-compliance=disabled -Dpycamera=disabled \
    -Dv4l2=false -Dudev=disabled -Dtracing=disabled -Dlibdw=disabled -Dlibunwind=disabled \
    -Dapps-output-dng=disabled -Ddocumentation=disabled -Dtest=false > $W/af-hal-setup.log 2>&1 \
    || { tail -30 $W/af-hal-setup.log; echo AF_HAL_SETUP_FAIL; exit 1; }
  ninja -C build-hal-af -j${J:-4} > $W/af-hal.log 2>&1 || { grep -E -B2 -A10 "error|FAILED" $W/af-hal.log | head -80; echo AF_HAL_FAIL; exit 1; }
  echo "hal warnings (non-subproject): $(grep 'warning:' $W/af-hal.log | grep -vc subprojects || true)"
  ls -la build-hal-af/src/android/libcamera-hal.so build-hal-af/src/ipa/simple/ipa_soft_simple.so; echo AF_HAL_OK
  ;;
*) echo "unknown step $s"; exit 2 ;;
esac; done
