#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Standalone compile check of A6LDisplaySwitcher (no `m`, nothing written in the tree): aapt2 compile+link (en + fr),
# javac -Xlint:all -Werror against the tree's system android.jar, d8. usage: build-check.sh <lineage-tree> [workdir]
set -eu
L=${1:?lineage tree}; W=${2:-/tmp/a6l-app-check}
APP=$(cd "$(dirname "$0")/.." && pwd)
JAR=$L/prebuilts/sdk/current/system/android.jar; AAPT2=$L/prebuilts/sdk/tools/linux/bin/aapt2
JDK=$L/prebuilts/jdk/jdk21/linux-x86/bin; R8=$L/prebuilts/r8/r8.jar
rm -rf "$W"; mkdir -p "$W"/{flat,gen,cls,dex}
# sources may carry CRLF on the Windows side: normalise a copy
cp -r "$APP" "$W/app"; find "$W/app" -type f \( -name '*.xml' -o -name '*.java' \) -exec sed -i 's/\r$//' {} +
"$AAPT2" compile --dir "$W/app/res" -o "$W/flat/res.zip"
"$AAPT2" link -I "$JAR" --manifest "$W/app/AndroidManifest.xml" --java "$W/gen" -o "$W/app.apk" "$W/flat/res.zip" --auto-add-overlay
unzip -l "$W/app.apk" | grep -q resources.arsc
"$AAPT2" dump resources "$W/app.apk" | grep -q '(fr)' && echo "fr resources linked"
"$JDK/javac" -Xlint:all,-options -Werror -source 17 -target 17 -cp "$JAR" -d "$W/cls" $(find "$W/app/src" "$W/gen" -name '*.java')
"$JDK/java" -cp "$R8" com.android.tools.r8.D8 --lib "$JAR" --min-api 30 --output "$W/dex" $(find "$W/cls" -name '*.class')
ls -l "$W/dex/classes.dex"
echo BUILD_CHECK PASS
