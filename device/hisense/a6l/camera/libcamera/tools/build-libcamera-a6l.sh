#!/bin/bash
# A6L: cross-build upstream libcamera (v0.7.2 + A6L sensor patch) for aarch64 Android (bionic, NDK r27c, API 30)
# with the simple pipeline, SoftISP (CPU debayer), simple IPA and the `cam` utility.
# Output prefix = /tmp/libcam1 (the recovery test location); a bundle is produced in $W/bundle.
# Runs entirely in /home/a6l/libcamera-work: does NOT touch the Lineage tree (a6l-lineage24) or its out/.
# Usage: build-libcamera-a6l.sh [step...]   steps: src openssl libevent libcamera bundle (default: all)
# lc2 (29 Sep 2026, phase 2): Android HAL build for the ROM, separate build dir/prefix (the recovery bundle is unchanged):
#   build-libcamera-a6l.sh src jpeg exif hal halpkg
#   jpeg/exif: static libjpeg-turbo 3.0.4 / libexif 0.6.25 in the sysroot (HAL JPEG encoder + EXIF);
#   hal: meson build-hal, --prefix=/vendor --libdir=lib64, -Dandroid=enabled -Dandroid_platform=generic, cam off;
#   halpkg: strip, re-sign the IPA, copy into $R/prebuilt/ (consumed by $R/Android.bp + libcamera-vendor.mk) + SHA256SUMS.
set -euo pipefail
W=/home/a6l/libcamera-work
R=/mnt/c/Users/Pierre/Desktop/A6L/device/hisense/a6l/camera/libcamera
NDK=/home/a6l/ndk/android-ndk-r27c
API=30
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
TAG=v0.7.2
OSSL=3.3.2
PREFIX=${PREFIX:-/tmp/libcam1}
# lc2 recovery bundle (29 Sep 2026): build in separate dirs so a concurrent HAL build (src-a6l/build-hal) is untouched:
#   PREFIX=/tmp/libcam2 SRCD=src-lc2 BLD=build-lc2 STG=stage-lc2 BUN=$W/bundle-lc2 build-libcamera-a6l.sh src libcamera bundle
SRCD=${SRCD:-src-a6l}; BLD=${BLD:-build-a6l}; STG=${STG:-stage}; BUN=${BUN:-$W/bundle}
# hi846 merge (29 Sep 2026): the HAL steps take their dirs from HSRC/HBLD/HSTG/HPKG (defaults = the lc2 dirs), e.g.
#   SRCD=src-h846 HSRC=src-h846 HBLD=build-hal-h846 HSTG=stage-hal-h846 HPKG=$W/halpkg-h846 build-libcamera-a6l.sh src hal halpkg
HSRC=${HSRC:-src-a6l}; HBLD=${HBLD:-build-hal}; HSTG=${HSTG:-stage-hal}; HPKG=${HPKG:-$W/halpkg}
J=${J:-4}
export PATH=$W/venv/bin:$PATH
cd $W   # never run python from /home/a6l (a stray dis.py there shadows the stdlib)
steps=${*:-src openssl libevent libcamera bundle}

for s in $steps; do case $s in
src)
  [ -d libcamera-src ] || git clone -q --depth 1 -b $TAG https://git.libcamera.org/libcamera/libcamera.git libcamera-src
  rm -rf $SRCD; git clone -q libcamera-src $SRCD
  git -C $SRCD checkout -q $TAG
  git -C $SRCD apply --whitespace=nowarn $R/patches/*.patch
  echo "SRC_OK $SRCD $(git -C $SRCD describe --tags) + $(ls $R/patches | wc -l) patch(es)"
  ;;
openssl)
  [ -f openssl-$OSSL.tar.gz ] || curl -sSL -o openssl-$OSSL.tar.gz https://github.com/openssl/openssl/releases/download/openssl-$OSSL/openssl-$OSSL.tar.gz
  rm -rf openssl-$OSSL; tar xzf openssl-$OSSL.tar.gz
  (cd openssl-$OSSL && export ANDROID_NDK_ROOT=$NDK PATH=$TC:$PATH &&
   ./Configure android-arm64 -D__ANDROID_API__=$API no-shared no-tests no-apps no-docs no-engine no-dso --prefix=$W/sysroot --libdir=lib >/dev/null &&
   make -j$J build_libs >/dev/null 2>&1 && make install_dev >/dev/null)
  ls -la sysroot/lib/libcrypto.a; echo OPENSSL_OK
  ;;
libevent)
  LEV=2.1.12-stable
  [ -f libevent-$LEV.tar.gz ] || curl -sSL -o libevent-$LEV.tar.gz https://github.com/libevent/libevent/releases/download/release-$LEV/libevent-$LEV.tar.gz
  rm -rf libevent-$LEV; tar xzf libevent-$LEV.tar.gz
  (cd libevent-$LEV && ./configure -q --host=aarch64-linux-android --prefix=$W/sysroot --disable-shared --enable-static \
     --disable-openssl --disable-mbedtls --disable-samples --disable-libevent-regress --disable-debug-mode --with-pic \
     CC=$TC/aarch64-linux-android$API-clang AR=$TC/llvm-ar RANLIB=$TC/llvm-ranlib >/dev/null &&
   make -j$J >/dev/null 2>&1 && make install >/dev/null 2>&1)
  ls sysroot/lib/pkgconfig; echo LIBEVENT_OK
  ;;
libcamera)
  cat > $W/a6l-android-aarch64.cross <<CROSS
[binaries]
c = '$TC/aarch64-linux-android$API-clang'
cpp = '$TC/aarch64-linux-android$API-clang++'
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = '$W/sysroot/lib/pkgconfig'
[built-in options]
cpp_link_args = ['-static-libstdc++']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
CROSS
  cp $W/a6l-android-aarch64.cross $R/a6l-android-aarch64.cross.generated
  rm -rf $BLD $STG
  meson setup $BLD $SRCD --cross-file $W/a6l-android-aarch64.cross \
    --prefix=$PREFIX --libdir=lib --buildtype=release -Db_ndebug=false \
    --force-fallback-for=yaml-0.1 \
    -Dpipelines=simple -Dipas=simple -Dsoftisp-gpu=disabled \
    -Dcam=enabled -Dcam-output-kms=disabled -Dcam-output-sdl2=disabled -Dcam-jpeg=disabled \
    -Dandroid=disabled -Dgstreamer=disabled -Dqcam=disabled -Dlc-compliance=disabled -Dpycamera=disabled \
    -Dv4l2=false -Dudev=disabled -Dtracing=disabled -Dlibdw=disabled -Dlibunwind=disabled \
    -Dapps-output-dng=disabled -Ddocumentation=disabled -Dtest=false 2>&1 | tail -60
  ninja -C $BLD -j$J 2>&1 | tail -15
  DESTDIR=$W/$STG meson install -C $BLD --no-rebuild >/dev/null
  find $STG -type f | sort; echo LIBCAMERA_OK
  ;;
bundle)
  B=$BUN; rm -rf $B; mkdir -p $B
  cp -a $STG$PREFIX/. $B/
  for f in $(find $B -type f \( -name '*.so*' -o -path '*/bin/*' -o -path '*/libexec/*' \)); do
    file $f | grep -q ELF && $TC/llvm-strip --strip-unneeded $f 2>/dev/null || true; done
  # the stripped IPA must keep a valid signature: re-sign after stripping
  for m in $(find $B/lib/libcamera -name 'ipa_*.so' 2>/dev/null); do
    $SRCD/src/ipa/ipa-sign.sh $BLD/src/ipa-priv-key.pem $m $m.sign 2>/dev/null && echo "resigned $(basename $m)"; done
  cp $R/data/*.yaml $B/share/libcamera/ipa/simple/ 2>/dev/null || true
  rm -rf $B/include $B/lib/pkgconfig $B/lib/*.a $B/bin/libcamera-bug-report
  openssl pkey -in $BLD/src/ipa-priv-key.pem -pubout -out $W/ipa-pub.pem 2>/dev/null
  for m in $(find $B/lib/libcamera -name 'ipa_*.so'); do
    openssl dgst -sha256 -verify $W/ipa-pub.pem -signature $m.sign $m >/dev/null && echo "IPA_SIGN_OK $(basename $m)" || { echo "IPA_SIGN_BAD $m"; exit 1; }; done
  cp $R/run-libcam.sh $R/tools/libcam_rgb_to_png.py $B/
  (cd $B && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS)
  $TC/llvm-readelf -d $B/bin/cam | grep -E "NEEDED|RUNPATH"
  find $B -type f | sort | xargs ls -la; echo BUNDLE_OK
  ;;
jpeg)
  JT=3.0.4
  [ -f libjpeg-turbo-$JT.tar.gz ] || curl -sSL -o libjpeg-turbo-$JT.tar.gz https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JT/libjpeg-turbo-$JT.tar.gz
  rm -rf libjpeg-turbo-$JT build-jpeg; tar xzf libjpeg-turbo-$JT.tar.gz
  cmake -S libjpeg-turbo-$JT -B build-jpeg -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=$API -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
    -DWITH_TURBOJPEG=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=$W/sysroot -DCMAKE_INSTALL_LIBDIR=lib >/dev/null &&
  ninja -C build-jpeg -j$J >/dev/null && ninja -C build-jpeg install >/dev/null
  ls -la sysroot/lib/libjpeg.a sysroot/lib/pkgconfig/libjpeg.pc; echo JPEG_OK
  ;;
exif)
  EX=0.6.25
  [ -f libexif-$EX.tar.gz ] || curl -sSL -o libexif-$EX.tar.gz https://github.com/libexif/libexif/releases/download/v$EX/libexif-$EX.tar.gz
  rm -rf libexif-$EX; tar xzf libexif-$EX.tar.gz
  (cd libexif-$EX && ./configure -q --host=aarch64-linux-android --prefix=$W/sysroot --disable-shared --enable-static \
     --disable-nls --disable-docs --with-pic CC=$TC/aarch64-linux-android$API-clang AR=$TC/llvm-ar RANLIB=$TC/llvm-ranlib >/dev/null &&
   make -j$J >/dev/null 2>&1 && make install >/dev/null 2>&1)
  ls -la sysroot/lib/libexif.a sysroot/lib/pkgconfig/libexif.pc; echo EXIF_OK
  ;;
hal)
  [ -f $W/a6l-android-aarch64.cross ] || { echo "run the libcamera step once first (cross file)"; exit 1; }
  rm -rf $HBLD $HSTG
  # libyuv is a cmake subproject: a cross build needs cmake named in the cross file
  # (libyuv's shared target also links -ljpeg: add the sysroot lib dir)
  sed -e "s#^\[binaries\]#[binaries]\ncmake = '$W/venv/bin/cmake'#" \
      -e "s#^cpp_link_args = \['-static-libstdc++'\]#cpp_link_args = ['-static-libstdc++', '-L$W/sysroot/lib']#" \
      $W/a6l-android-aarch64.cross > $W/a6l-android-hal.cross
  # ... and cmake must not look for an NDK layout (system android): build the static libyuv as plain Linux/aarch64
  printf "[cmake]\nCMAKE_SYSTEM_NAME = 'Linux'\nCMAKE_SYSTEM_PROCESSOR = 'aarch64'\n" >> $W/a6l-android-hal.cross
  meson setup $HBLD $HSRC --cross-file $W/a6l-android-hal.cross \
    --prefix=/vendor --libdir=lib64 --sysconfdir=etc --datadir=share --libexecdir=libexec --buildtype=release -Db_ndebug=false \
    --force-fallback-for=yaml-0.1 \
    -Dpipelines=simple -Dipas=simple -Dsoftisp-gpu=disabled \
    -Dcam=disabled -Dandroid=enabled -Dandroid_platform=generic \
    -Dgstreamer=disabled -Dqcam=disabled -Dlc-compliance=disabled -Dpycamera=disabled \
    -Dv4l2=false -Dudev=disabled -Dtracing=disabled -Dlibdw=disabled -Dlibunwind=disabled \
    -Dapps-output-dng=disabled -Ddocumentation=disabled -Dtest=false > $W/hal-setup.log 2>&1 || { tail -30 $W/hal-setup.log; echo HAL_SETUP_FAIL; exit 1; }
  grep -E "Android support|IPAs|Pipelines|libyuv|libjpeg|libexif" $W/hal-setup.log | cut -c1-150 || true
  ninja -C $HBLD -j$J > $W/hal-ninja.log 2>&1 || { grep -E -B2 -A8 "error|FAILED" $W/hal-ninja.log | head -80; echo HAL_BUILD_FAIL; exit 1; }
  echo "hal warnings: $(grep -c 'warning:' $W/hal-ninja.log)"; grep 'warning:' $W/hal-ninja.log | grep -v subprojects | head -10 || true
  DESTDIR=$W/$HSTG meson install -C $HBLD --no-rebuild >/dev/null
  find $HSTG -type f | sort; echo HAL_OK
  ;;
halpkg)
  # strip + sign in a WSL-local staging dir (llvm-strip cannot rewrite files in place on /mnt/c), then copy to $R/prebuilt
  P=$R/prebuilt; K=$HPKG; rm -rf $K; mkdir -p $K/lib64/hw $K/lib64/libcamera/ipa $K/share/libcamera/ipa/simple $K/libexec/libcamera
  S=$W/$HSTG/vendor
  # android host: meson installs unversioned sonames (libcamera.so / libcamera-base.so)
  cp $S/lib64/libcamera.so $S/lib64/libcamera-base.so $K/lib64/
  cp $S/lib64/libcamera-hal.so $K/lib64/hw/camera.libcamera.so
  cp $S/lib64/libcamera/ipa/ipa_soft_simple.so $K/lib64/libcamera/ipa/
  cp $S/libexec/libcamera/soft_ipa_proxy $K/libexec/libcamera/
  for f in $K/lib64/*.so $K/lib64/hw/*.so $K/lib64/libcamera/ipa/*.so $K/libexec/libcamera/soft_ipa_proxy; do $TC/llvm-strip --strip-unneeded $f; done
  m=$K/lib64/libcamera/ipa/ipa_soft_simple.so
  $HSRC/src/ipa/ipa-sign.sh $HBLD/src/ipa-priv-key.pem $m $m.sign && echo "resigned $(basename $m)"
  openssl pkey -in $HBLD/src/ipa-priv-key.pem -pubout -out $W/ipa-hal-pub.pem 2>/dev/null
  openssl dgst -sha256 -verify $W/ipa-hal-pub.pem -signature $m.sign $m >/dev/null && echo "IPA_SIGN_OK $(basename $m)" || { echo IPA_SIGN_BAD; exit 1; }
  # the public key compiled into libcamera.so must be the one matching the signing key
  python3 - $W/ipa-hal-pub.pem $K/lib64/libcamera.so <<'PY' || { echo IPA_PUBKEY_NOT_IN_LIBCAMERA; exit 1; }
import sys, base64
der = base64.b64decode(''.join(l for l in open(sys.argv[1]).read().splitlines() if not l.startswith('-----')))
sys.exit(0 if der in open(sys.argv[2], 'rb').read() else 1)
PY
  echo IPA_PUBKEY_MATCH
  cp $S/share/libcamera/ipa/simple/*.yaml $K/share/libcamera/ipa/simple/; cp $R/data/*.yaml $K/share/libcamera/ipa/simple/
  for f in $K/lib64/*.so $K/lib64/hw/*.so $K/lib64/libcamera/ipa/*.so; do echo "$(basename $f): $($TC/llvm-readelf -d $f | grep -E 'NEEDED|SONAME' | sed 's/.*\[\(.*\)\]/\1/' | tr '\n' ' ')"; done
  grep -a -o "/vendor/[a-z0-9/._-]*libcamera[a-z0-9/._-]*" $K/lib64/libcamera.so $K/lib64/hw/camera.libcamera.so | sort -u | head
  { echo "libcamera $(git -C $HSRC describe --tags) + A6L patches: $(ls $R/patches | tr '\n' ' ')"; echo "built $(date -Iseconds) with NDK r27c API $API, meson $HBLD (prefix /vendor, libdir lib64)";
    echo "IPA signing key: $HBLD/src/ipa-priv-key.pem (public key embedded in libcamera.so); ipa_soft_simple.so.sign regenerated after strip"; } > $K/BUILD-INFO.txt
  (cd $K && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS; cat SHA256SUMS; du -sh .)
  mkdir -p $P; cp -r $K/. $P/
  (cd $P && sha256sum -c --quiet SHA256SUMS) && echo PREBUILT_COPY_OK
  echo HALPKG_OK
  ;;
esac; done
