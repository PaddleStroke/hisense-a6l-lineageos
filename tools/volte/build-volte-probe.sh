#!/usr/bin/env bash
# Build the read-only VoLTE/IMS QMI probe (static aarch64, NDK r27c) + host unit test.
# Run by the relay as a6l (or root): output in firmware/extracted/volte-20260925/.
exec < /dev/null
set -eo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L
SRC=$R/tools/volte
O=/home/a6l/volte-build; mkdir -p $O
NDK=/home/a6l/ndk/android-ndk-r27c
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang
cp $SRC/volte-probe.c $SRC/test-volte-probe.c $O/
cd $O
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -o t test-volte-probe.c && ./t
$CC -std=c11 -D_GNU_SOURCE -O2 -static -Wall -Wextra -Werror volte-probe.c -o volte-probe
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip volte-probe
file volte-probe
mkdir -p $R/firmware/extracted/volte-20260925
cp volte-probe $R/firmware/extracted/volte-20260925/volte-probe
cd $R/firmware/extracted/volte-20260925 && sha256sum volte-probe | tee SHA256SUMS
