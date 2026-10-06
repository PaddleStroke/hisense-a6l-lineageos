#!/usr/bin/env bash
# r6d (30 Sep 2026, docs/rom-r6d-20260930.md): load the vendor Mesa EGL/GLES set of a BUILT image, both ABIs, with the real
# bionic linker/libc of the same build, under qemu-arm / qemu-aarch64 user mode. WSL, offline, no phone.
# usage: run-egl-load-test.sh <vendor.erofs|vendor.img> <product out dir (apex/com.android.runtime, system/lib*)> <work dir>
#   -> A6L_EGL_LOAD_TEST PASS|FAIL  (per ABI: A6L_EGL_LOAD_OK 32-bit / 64-bit)
set -uo pipefail
VI=$1; O=$2; WK=$3; D=$(cd "$(dirname "$0")" && pwd); TC=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
FSCK=/home/a6l/android/a6l-lineage24/out/host/linux-x86/bin/fsck.erofs
rm -rf $WK; mkdir -p $WK/root/system $WK/root/apex $WK/root/system/bin
# the vendor image under test (what the phone will get), extracted
if [ "$(head -c 1028 $VI | tail -c 4 | od -An -tx1 | tr -d ' ')" = e2e1f5e0 ]; then $FSCK --extract=$WK/root/vendor $VI > $WK/fsck.log 2>&1 || { tail $WK/fsck.log; exit 1; }
else $FSCK --extract=$WK/root/vendor $VI > $WK/fsck.log 2>&1 || { echo "cannot extract $VI"; tail -3 $WK/fsck.log; exit 1; }; fi
# bionic (linker, libc/libm/libdl) + the LL-NDK/system libs Mesa needs, from the same build
cp -a $O/apex/com.android.runtime $WK/root/apex/
for a in lib lib64; do mkdir -p $WK/root/system/$a; ( cd $O/system/$a && find . -maxdepth 1 -name '*.so' ! -name 'libc.so' ! -name 'libm.so' ! -name 'libdl.so' -exec cp -L {} $WK/root/system/$a/ \; 2>/dev/null )
  for l in $(ls $O/apex/com.android.runtime/$a/bionic); do ln -sf ../../apex/com.android.runtime/$a/bionic/$l $WK/root/system/$a/$l; done; done
cp -L $O/apex/com.android.runtime/bin/linker $WK/root/system/bin/linker; cp -L $O/apex/com.android.runtime/bin/linker64 $WK/root/system/bin/linker64
# qemu-user resolves symlinks on the host: no absolute links inside the root (the apex copy is dereferenced)
rm -rf $WK/root/apex/com.android.runtime; cp -rL $O/apex/com.android.runtime $WK/root/apex/
$TC/armv7a-linux-androideabi34-clang -O1 -o $WK/root/a6l_egl_load32 $D/a6l_egl_load.c -ldl || exit 1
$TC/aarch64-linux-android34-clang -O1 -o $WK/root/a6l_egl_load64 $D/a6l_egl_load.c -ldl || exit 1
ok=0
for x in "32 qemu-arm lib" "64 qemu-aarch64 lib64"; do set -- $x
  # vendor libs first (sphal-like: /vendor/lib{,64}), then system (LL-NDK), no linkerconfig -> default paths + LD_LIBRARY_PATH
  # own PID namespace: 32-bit bionic aborts for pids > 65535 (pthread_mutex_t), WSL's pids are larger
  QEMU_LD_PREFIX=$WK/root timeout 300 unshare --user --map-root-user --pid --fork $2 -L $WK/root -E LD_LIBRARY_PATH=/vendor/$3:/system/$3 -E ANDROID_DATA=/tmp -E ANDROID_ROOT=/system \
    $WK/root/a6l_egl_load$1 /vendor/$3/egl/libEGL_mesa.so /vendor/$3/egl/libGLESv1_CM_mesa.so /vendor/$3/egl/libGLESv2_mesa.so > $WK/run$1.log 2>&1
  echo "rc=$? $1-bit"; grep -v '^WARNING: linker' $WK/run$1.log | tail -6
  grep -q "A6L_EGL_LOAD_OK $1-bit" $WK/run$1.log && ok=$((ok + 1))
done
echo "A6L_EGL_LOAD_TEST $([ $ok = 2 ] && echo PASS || echo FAIL)"
