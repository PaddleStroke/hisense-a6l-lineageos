#!/bin/bash
# btcall static + host checks (29 Sep 2026, docs/android-bt-audio-20260929.md). usage: check-btcall.sh [lineage-tree]
# Without a tree: bridge policy generated/up to date + structure, init rc, product wiring, sepolicy contexts, default off,
# a6l-q6voiced host tests (gcc). With a tree (WSL): + XSD validation, audio patch series 0001..0004 on the pristine
# hardware/interfaces HEAD in pipeline order (and every patch still reverse-applies = the pipeline's idempotency check),
# a6l-audio-route host tests (real libaudioroute). Prints BTCALL_CHECK PASS|FAIL.
set -u
H=$(cd "$(dirname "$0")/.." && pwd)            # audio/bluetooth/btcall
D=$(cd "$H/../../.." && pwd)                   # device/hisense/a6l
L=${1:-}
fails=0
ok() { echo "PASS $1"; }; ko() { echo "FAIL $1"; fails=$((fails+1)); }
python3 "$H/gen-btcall-policy.py" --check && ok "bridge policy up to date with audio/audio_policy_configuration.xml" || ko "bridge policy stale"
python3 - "$D" "$L" <<'PY' || fails=$((fails+1))
import os, re, subprocess, sys, tempfile, xml.etree.ElementTree as ET
D, L = sys.argv[1:3]
bad = []
def check(c, m):
    print(("PASS " if c else "FAIL ") + m)
    if not c: bad.append(m)
H = os.path.join(D, "audio/bluetooth/btcall")
pol = os.path.join(H, "audio_policy_configuration_btcall.xml")
root = ET.parse(pol).getroot()
mods = {m.get("name"): m for m in root.iter("module")}
pr = mods["primary"]
att = [i.text for i in pr.find("attachedDevices").iter("item")]
check({"Telephony Tx", "Telephony Rx"} <= set(att), "Telephony Tx/Rx attached in the primary module")
dev = {p.get("tagName"): p for p in pr.iter("devicePort")}
mix = {p.get("name"): p for p in pr.iter("mixPort")}
check(dev.get("Telephony Tx") is not None and dev["Telephony Tx"].get("type") == "AUDIO_DEVICE_OUT_TELEPHONY_TX" and dev["Telephony Tx"].get("role") == "sink", "devicePort Telephony Tx OUT_TELEPHONY_TX sink")
check(dev.get("Telephony Rx") is not None and dev["Telephony Rx"].get("type") == "AUDIO_DEVICE_IN_TELEPHONY_RX" and dev["Telephony Rx"].get("role") == "source", "devicePort Telephony Rx IN_TELEPHONY_RX source")
check(mix.get("telephony tx") is not None and mix["telephony tx"].get("role") == "source" and mix.get("telephony rx") is not None and mix["telephony rx"].get("role") == "sink", "mix ports telephony tx (source) / telephony rx (sink)")
for n in ("telephony tx", "telephony rx"):
    for p in mix[n].iter("profile"):
        check(set(p.get("samplingRates").split()) <= {"8000", "16000"} and "MONO" in p.get("channelMasks"), f"'{n}' 8/16 kHz mono (in-call record/music)")
routes = {r.get("sink"): [s.strip() for s in r.get("sources").split(",")] for r in pr.iter("route")}
for s in ("Earpiece", "Speaker", "Wired Headset", "Wired Headphones"):
    check("Telephony Rx" in routes.get(s, []) and "primary output" in routes.get(s, []), f"HW route Telephony Rx -> {s} (calls on the phone stay HW patches, no SW loop through the primary output)")
check(set(routes.get("Telephony Tx", [])) == {"telephony tx", "Built-In Mic", "Wired Headset Mic"}, "Telephony Tx <- telephony tx + phone mics")
check(routes.get("telephony rx") == ["Telephony Rx"], "telephony rx <- Telephony Rx only")
check(not any("Telephony Rx" in v for k, v in routes.items() if k in mix and k != "telephony rx"), "no other mix fed by Telephony Rx")
names = set(dev) | set(mix)
check(all(k in names for k in routes) and all(s in names for v in routes.values() for s in v), "primary routes reference declared ports")
# every other module identical to the product policy
prod = ET.parse(os.path.join(D, "audio/audio_policy_configuration.xml")).getroot()
pm = {m.get("name"): m for m in prod.iter("module")}
for n in pm:
    if n != "primary":
        check(ET.tostring(pm[n]) == ET.tostring(mods[n]), f"module {n} unchanged")
bt = mods["bluetooth"]
btr = {r.get("sink"): r.get("sources") for r in bt.iter("route")}
check(btr.get("BT SCO Headset") == "hfp output" and btr.get("hfp input") == "BT SCO Headset Mic", "SW bridge ends exist: hfp output -> BT SCO Headset, BT SCO Headset Mic -> hfp input")
# init / product / sepolicy
rc = open(os.path.join(H, "init.a6l.btcall.rc")).read()
check(re.search(r"^on early-boot && property:persist\.vendor\.a6l\.btcall\.bridge=1\n    mount none /vendor/etc/audio_policy_configuration_btcall\.xml /vendor/etc/audio_policy_configuration\.xml bind$", rc, re.M) is not None, "init: bind mount at early-boot only with persist.vendor.a6l.btcall.bridge=1")
mk = open(os.path.join(D, "audio/bluetooth/bt-audio.mk")).read()
check("btcall/audio_policy_configuration_btcall.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration_btcall.xml" in mk, "bt-audio.mk installs the bridge policy")
check("btcall/init.a6l.btcall.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.btcall.rc" in mk, "bt-audio.mk installs init.a6l.btcall.rc")
defaults = []
for dp, _, fs in os.walk(D):
    for f in fs:
        if f.endswith(".mk"):
            defaults += re.findall(r"^\s+(persist\.vendor\.a6l\.btcall\.\w+=\S*)", open(os.path.join(dp, f), errors="replace").read(), re.M)
check(not defaults, "default OFF: no product sets persist.vendor.a6l.btcall.*" + (f" ({defaults})" if defaults else ""))
pc = open(os.path.join(D, "audio/sepolicy/property_contexts")).read()
check(re.search(r"^persist\.vendor\.a6l\.btcall\.\s+u:object_r:vendor_a6l_audio_prop:s0", pc, re.M) is not None, "property_contexts: persist.vendor.a6l.btcall. -> vendor_a6l_audio_prop")
te = open(os.path.join(D, "audio/sepolicy/hal_audio_default.te")).read() + open(os.path.join(D, "audio/sepolicy/a6l_audio_route.te")).read() + open(os.path.join(D, "kvoice/q6voiced/sepolicy/a6l_q6voiced.te")).read()
check("set_prop(hal_audio_default, vendor_a6l_audio_prop)" in te and "get_prop(a6l_audio_route, vendor_a6l_audio_prop)" in te and "get_prop(a6l_q6voiced, vendor_a6l_audio_prop)" in te, "readers: audio HAL, a6l-audio-route, a6l-q6voiced")
check("proc_asound" in te, "both daemons may read /proc/asound (bridge detection)")
check("allow init vendor_configs_file:file mounton;" in open(os.path.join(D, "audio/sepolicy/btcall.te")).read(), "init may bind-mount the policy file")
mp = open(os.path.join(D, "audio/mixer_paths_a6l.xml")).read()
check('<path name="voice-bridge">' in mp and '<path name="bridge-mic">' in mp, "mixer paths voice-bridge / bridge-mic")
if L:
    xsd = os.path.join(L, "hardware/interfaces/audio/aidl/default/config/audioPolicy/audio_policy_configuration.xsd")
    txt = open(pol).read().replace('<xi:include href="audio_policy_volumes.xml"/>', '<volumes/>')
    txt = re.sub(r'<xi:include[^>]*/>', '', txt)
    txt = re.sub(r'<profile name=""', '<profile', txt)
    with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False) as f:
        f.write(txt)
    r = subprocess.run(["xmllint", "--noout", "--schema", xsd, f.name], capture_output=True, text=True)
    check(r.returncode == 0, "bridge policy validates against the tree's AIDL audio_policy_configuration.xsd")
    if r.returncode: print(r.stderr[-1500:])
    os.unlink(f.name)
sys.exit(1 if bad else 0)
PY
W=$(mktemp -d); trap 'rm -rf $W' EXIT
if gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -O1 -g -fsanitize=address,undefined -pthread \
     -o $W/q6vt "$D/kvoice/q6voiced/tests/q6voiced_tests.c" && $W/q6vt > $W/q6.log 2>&1; then ok "a6l-q6voiced host tests ($(tail -1 $W/q6.log))"; else ko "a6l-q6voiced host tests"; tail -5 $W/q6.log; fi
if [ -n "$L" ]; then
  HI=$L/hardware/interfaces; mkdir -p $W/seq; ( cd $W/seq && git init -q ) ; sfail=0
  for f in $(cat "$D"/audio/patches/*.patch | tr -d '\r' | sed -n 's#^+++ b/\([^\t ]*\).*#\1#p' | sort -u); do mkdir -p $W/seq/$(dirname $f); git -C $HI -c safe.directory='*' show HEAD:$f > $W/seq/$f || sfail=1; done
  for p in "$D"/audio/patches/*.patch; do tr -d '\r' < $p > $W/x.patch; ( cd $W/seq && git apply $W/x.patch ) || { sfail=1; echo "  apply failed: $(basename $p)"; }; done
  for p in "$D"/audio/patches/*.patch; do tr -d '\r' < $p > $W/x.patch; ( cd $W/seq && git apply --reverse --check $W/x.patch 2>/dev/null ) || { sfail=1; echo "  no longer reverse-applies: $(basename $p)"; }; done
  [ $sfail = 0 ] && ok "audio patches $(ls "$D"/audio/patches/*.patch | xargs -n1 basename | tr '\n' ' ')apply in order on HEAD and each reverse-applies" || ko "audio patch series"
  grep -q "a6lBtcallPcm" $W/seq/audio/aidl/default/primary/StreamPrimary.cpp && ok "patched StreamPrimary has the btcall hook" || ko "btcall hook missing"
  if bash "$D/audio/route/tests/run-tests.sh" "$L" > $W/route.log 2>&1; then ok "a6l-audio-route host tests ($(grep -h "TESTS PASS" $W/route.log | tr '\n' ' '))"; else ko "a6l-audio-route host tests"; tail -5 $W/route.log; fi
fi
[ $fails = 0 ] && echo "BTCALL_CHECK PASS" || echo "BTCALL_CHECK FAIL $fails"
exit $([ $fails = 0 ] && echo 0 || echo 1)
