#!/usr/bin/env bash
# A6L release signing keys (release-prep, 27 Sep 2026) - Lineage wiki "Signing builds" procedure, one command.
# !!! NOT RUN BY THE AGENTS. Keys are FOREVER: every future OTA must be signed with the same keys, or users must wipe /
# run the Lineage key-migration script. Pierre decides the subject, password policy and where the backup lives.
#
# usage (WSL, as a6l, from anywhere):
#   A6L_KEY_SUBJECT='/C=FR/ST=.../L=.../O=A6L LineageOS/OU=release/CN=A6L LineageOS/emailAddress=...' \
#     bash tools/release/gen-release-keys.sh [--password] [--apex-from <target_files.zip>]
#   --password           : make_key asks for a password per key (then signing needs ANDROID_PW_FILE); default = no password
#   --apex-from <zip>    : also create keys for every APEX listed in META/apexkeys.txt of that target-files (so none is missed)
#   A6L_KEYDIR           : default /home/a6l/.android-certs (OUTSIDE the repo; never commit, never copy to the Windows side)
#   A6L_KEYS_ADD_MISSING=1 : directory may already exist; only keys that do not exist yet are created (e.g. a new APEX)
# Output: <keydir>/{releasekey,platform,shared,media,networkstack,nfc,sdk_sandbox,bluetooth,cyngn-app,testcert,verity}.{pk8,x509.pem},
#         testkey.* -> releasekey.* (wiki), <apex>.{pk8,x509.pem,pem} (RSA 4096) per APEX, avb.pem + avb_pkmd.bin (RSA 4096,
#         for a future AVB/vbmeta; unused while BOARD_AVB_ENABLE=false), fingerprints.txt (public cert SHA-256 only).
# Nothing secret is printed.
set -euo pipefail
L=${A6L_TREE:-/home/a6l/android/a6l-lineage24}
K=${A6L_KEYDIR:-/home/a6l/.android-certs}
PW=0; APEX_FROM=""
while [ $# -gt 0 ]; do case "$1" in --password) PW=1;; --apex-from) APEX_FROM=$2; shift;; *) echo "unknown arg $1"; exit 2;; esac; shift; done
: "${A6L_KEY_SUBJECT:?set A6L_KEY_SUBJECT (e.g. /C=FR/O=A6L LineageOS/CN=A6L LineageOS) - it is embedded in every certificate}"
case "$K" in /mnt/c/*|*/Desktop/A6L*) echo "refusing: key dir $K is on the Windows side / inside the repo"; exit 1;; esac
[ -x $L/development/tools/make_key ] || { echo "make_key not found in $L"; exit 1; }
umask 077
if [ -e "$K" ] && [ -n "$(ls -A "$K" 2>/dev/null)" ] && [ "${A6L_KEYS_ADD_MISSING:-0}" != 1 ]; then
  echo "refusing: $K already contains keys (keys are forever). A6L_KEYS_ADD_MISSING=1 to add only missing ones."; exit 1; fi
mkdir -p "$K"; chmod 700 "$K"
mk() {  # mk <make_key> <name> <subject>
  [ -e "$K/$2.pk8" ] && { echo "exists: $2"; return 0; }
  # make_key exits 1 even on success (trailing wait), so check the files instead of its exit code
  if [ $PW = 1 ]; then "$1" "$K/$2" "$3" || true; else echo "" | "$1" "$K/$2" "$3" > /dev/null || true; fi
  [ -s "$K/$2.pk8" ] && [ -s "$K/$2.x509.pem" ] && openssl x509 -in "$K/$2.x509.pem" -noout >/dev/null 2>&1 || { echo "FAILED: $2"; exit 1; }
  echo "created: $2"
}
# 1. platform certificates (wiki list; testkey = releasekey)
for c in bluetooth cyngn-app media networkstack nfc platform releasekey sdk_sandbox shared testcert verity; do
  mk $L/development/tools/make_key $c "$A6L_KEY_SUBJECT"; done
ln -sf releasekey.pk8 "$K/testkey.pk8"; ln -sf releasekey.x509.pem "$K/testkey.x509.pem"
# 2. APEX keys: RSA 4096 (container cert + payload .pem)
cp $L/development/tools/make_key "$K/make_key"; sed -i 's|2048|4096|g' "$K/make_key"; chmod 700 "$K/make_key"
APEXES=$(tr -d '\r' < "$(dirname "$0")/apex-list.txt" | grep -v '^#' | awk 'NF{print $1}')
if [ -n "$APEX_FROM" ]; then
  APEXES="$APEXES $(unzip -p "$APEX_FROM" META/apexkeys.txt | grep -v 'private_key="PRESIGNED"' | sed -n 's/^name="\([^"]*\)\.\(c\)\{0,1\}apex".*/\1/p')"; fi
for a in $(echo $APEXES | tr ' ' '\n' | sort -u); do
  subj=$(echo "$A6L_KEY_SUBJECT" | sed "s#/CN=[^/]*#/CN=$a#"); case "$subj" in */CN=$a*) ;; *) subj="$subj/CN=$a";; esac
  mk "$K/make_key" "$a" "$subj"
  if [ ! -e "$K/$a.pem" ]; then
    if [ $PW = 1 ]; then openssl pkcs8 -in "$K/$a.pk8" -inform DER -out "$K/$a.pem"; else openssl pkcs8 -in "$K/$a.pk8" -inform DER -nocrypt -out "$K/$a.pem"; fi
  fi
done
# 3. AVB key (future use only; see docs/release-prep-20260927.md 1.3)
if [ ! -e "$K/avb.pem" ]; then
  openssl genrsa -out "$K/avb.pem" 4096 2> /dev/null
  python3 $L/external/avb/avbtool.py extract_public_key --key "$K/avb.pem" --output "$K/avb_pkmd.bin"
  echo "created: avb (RSA 4096)"; fi
chmod 600 "$K"/*.pk8 "$K"/*.pem 2>/dev/null || true
# 4. public fingerprints only (for the doc / to recognise the keys later)
( cd "$K" && for c in *.x509.pem; do [ -L "$c" ] && continue; printf '%s %s\n' "$(openssl x509 -in "$c" -noout -fingerprint -sha256 | cut -d= -f2)" "$c"; done ) > "$K/fingerprints.txt"
echo "A6L_KEYS_DONE dir=$K certs=$(ls "$K"/*.x509.pem | wc -l) apex_payload_keys=$(ls "$K"/*.pem | grep -vc x509) (fingerprints: $K/fingerprints.txt)"
echo "NEXT: back up $K OFFLINE (encrypted USB / password manager). Losing it = no more OTA updates for installed phones."
