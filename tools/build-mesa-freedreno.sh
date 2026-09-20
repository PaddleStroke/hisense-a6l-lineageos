#!/usr/bin/env bash
# Cross-build Mesa (freedreno gallium, EGL/GLES, Android platform) for the A6L Adreno 512 with the NDK.
# Offline build experiment; output stays under ~/mesa-a6l. The Lineage tree's external/mesa3d is used read-only.
set -eo pipefail
NDK=$HOME/ndk/android-ndk-r27c; TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin; API=34
SRC=$HOME/android/a6l-lineage24/external/mesa3d; OUT=$HOME/mesa-a6l; mkdir -p $OUT
export PATH=$HOME/venv-mesa/bin:$PATH
cp -r --reflink=auto $SRC $OUT/src 2>/dev/null || true   # wraps download into the copy, never into the tree
cat > $OUT/cross.ini <<X
[binaries]
ar = '$TC/llvm-ar'
c = ['$TC/aarch64-linux-android$API-clang']
cpp = ['$TC/aarch64-linux-android$API-clang++']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$TC/llvm-strip'
pkg-config = '$HOME/venv-mesa/lib/python3.12/site-packages/pkgconf/.bin/pkgconf'
[built-in options]
cpp_link_args = ['-static-libstdc++']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$OUT/sysroot/lib/pkgconfig'
X
# libdrm first (static, freedreno only) so Mesa's pkg-config lookup succeeds.
if [ ! -f $OUT/sysroot/lib/pkgconfig/libdrm.pc ]; then
  rm -rf $OUT/libdrm-src $OUT/libdrm-build; cp -r $HOME/android/a6l-lineage24/external/libdrm $OUT/libdrm-src
  meson setup $OUT/libdrm-build $OUT/libdrm-src --cross-file $OUT/cross.ini --prefix=$OUT/sysroot --libdir=lib -Ddefault_library=static \
    -Dfreedreno=enabled -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Dtests=false -Dman-pages=disabled -Dvalgrind=disabled -Dcairo-tests=disabled -Detnaviv=disabled -Dexynos=disabled -Dtegra=disabled -Dvc4=disabled -Domap=disabled > $OUT/libdrm-setup.log 2>&1 || { tail -n 15 $OUT/libdrm-setup.log; exit 1; }
  ninja -C $OUT/libdrm-build install > $OUT/libdrm-build.log 2>&1 || { tail -n 20 $OUT/libdrm-build.log; exit 1; }
fi
rm -rf $OUT/build
cd $OUT/src
meson setup $OUT/build --cross-file $OUT/cross.ini --wrap-mode=default \
  -Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
  -Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=msm -Degl=enabled -Dgles1=enabled -Dgles2=enabled \
  -Dgbm=disabled -Dglx=disabled -Dllvm=disabled -Dshared-glapi=enabled -Dcpp_rtti=false -Dbuildtype=release \
  -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dandroid-strict=false > $OUT/setup.log 2>&1 || { tail -n 25 $OUT/setup.log; exit 1; }
ninja -C $OUT/build -j12 > $OUT/build.log 2>&1 || { grep -m5 -B2 -A12 "FAILED" $OUT/build.log; exit 1; }
find $OUT/build -name "*.so*" -type f | head; echo A6L_MESA_FREEDRENO_BUILD_PASS
