#!/bin/bash
# bt-audio static checks (29 Sep 2026). usage: check-bt-audio.sh [lineage-tree]
# Host-only: policy XML structure (+ XSD validation and property_contexts types when a Lineage tree is given),
# bt-audio.mk properties, and that the product used by tools/rom-v2-pipeline.sh (lineage_gsi_a6l) reaches bt-audio.mk.
set -u
D=$(cd "$(dirname "$0")/../../.." && pwd)   # device/hisense/a6l
R=$(cd "$D/../../.." && pwd)
L=${1:-}
exec python3 - "$D" "$R" "$L" <<'PY'
import os, re, subprocess, sys, tempfile, xml.etree.ElementTree as ET
D, R, L = sys.argv[1:4]
fails = []
def check(c, msg):
    print(("PASS " if c else "FAIL ") + msg)
    if not c: fails.append(msg)

pol = os.path.join(D, "audio/audio_policy_configuration.xml")
root = ET.parse(pol).getroot()
mods = {m.get("name"): m for m in root.iter("module")}
check(set(mods) >= {"primary", "r_submix", "bluetooth"}, "modules primary/r_submix/bluetooth (APEX VINTF declares all three)")
bt = mods["bluetooth"]
mix = {p.get("name"): p for p in bt.iter("mixPort")}
dev = {p.get("tagName"): p for p in bt.iter("devicePort")}
routes = [(r.get("sink"), [s.strip() for s in r.get("sources").split(",")]) for r in bt.iter("route")]
for n, role in (("a2dp output", "source"), ("hfp output", "source"), ("hfp input", "sink")):
    check(n in mix and mix[n].get("role") == role, f"mixPort '{n}' role={role}")
    check(n in mix and len(list(mix[n].iter("profile"))) == 0, f"mixPort '{n}' dynamic (profile from the negotiated BT codec)")
want = {"BT SCO": ("AUDIO_DEVICE_OUT_BLUETOOTH_SCO", "sink"),
        "BT SCO Headset": ("AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET", "sink"),
        "BT SCO Car Kit": ("AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT", "sink"),
        "BT SCO Headset Mic": ("AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET", "source"),
        "BT A2DP Out": ("AUDIO_DEVICE_OUT_BLUETOOTH_A2DP", "sink")}
for t, (ty, role) in want.items():
    check(t in dev and dev[t].get("type") == ty and dev[t].get("role") == role, f"devicePort '{t}' {ty}")
for t in ("BT SCO", "BT SCO Headset", "BT SCO Car Kit", "BT SCO Headset Mic"):
    for p in dev[t].iter("profile"):
        rates = set(p.get("samplingRates").split())
        check(rates <= {"8000", "16000", "32000"} and "MONO" in p.get("channelMasks"),
              f"'{t}' profile within HfpSoftwareAudioProvider limits (8/16/32 kHz mono)")
for s in ("BT SCO", "BT SCO Headset", "BT SCO Car Kit"):
    check((s, ["hfp output"]) in routes, f"route hfp output -> {s}")
check(("hfp input", ["BT SCO Headset Mic"]) in routes, "route BT SCO Headset Mic -> hfp input")
allnames = set(mix) | set(dev)
check(all(s in allnames for r in routes for s in r[1]) and all(r[0] in allnames for r in routes), "bluetooth routes reference declared ports")
check(not any("BLE" in (p.get("type") or "") for p in bt.iter("devicePort")), "no LE audio ports (LE audio off)")
tags = [p.get("tagName") for p in root.iter("devicePort")]
check(len(tags) == len(set(tags)), "devicePort tagNames unique across modules")

mk = open(os.path.join(D, "audio/bluetooth/bt-audio.mk")).read()
props = dict(re.findall(r"^\s+([a-z0-9_.]+)=(\S+?)(?:\s*\\)?$", mk, re.M))
exp = {"bluetooth.profile.a2dp.source.enabled": "true", "bluetooth.profile.hfp.ag.enabled": "true",
       "bluetooth.profile.avrcp.target.enabled": "true", "bluetooth.profile.bap.unicast.client.enabled": "false",
       "persist.bluetooth.a2dp_offload.disabled": "true", "ro.bluetooth.a2dp_offload.supported": "false",
       "bluetooth.sco.managed_by_audio": "true", "bluetooth.hfp.software_datapath.enabled": "true",
       "persist.bluetooth.leaudio_offload.disabled": "true"}
for k, v in exp.items():
    check(props.get(k) == v, f"bt-audio.mk {k}={v}")
check("PRODUCT_VENDOR_PROPERTIES" in mk, "props go to the vendor build.prop (vendor_init may set bluetooth_config_prop / a2dp_offload_prop)")

rom = open(os.path.join(D, "rom/rom.mk")).read()
check(re.search(r"^\$\(call inherit-product, device/hisense/a6l/audio/bluetooth/bt-audio\.mk\)", rom, re.M) is not None, "rom/rom.mk inherits bt-audio.mk")
prod = open(os.path.join(D, "lineage_gsi_a6l.mk")).read()
check("inherit-product, device/hisense/a6l/rom/rom.mk" in prod, "lineage_gsi_a6l.mk inherits rom/rom.mk")
check("android.hardware.bluetooth-service.default" in rom, "HCI HAL in rom.mk PRODUCT_PACKAGES")
check("com.android.hardware.audio" in prod, "AIDL example audio HAL APEX (hosts the BluetoothAudio provider) in the product")
pipe = open(os.path.join(R, "tools/rom-v2-pipeline.sh")).read()
check(re.search(r"for d in [^;]*\baudio\b[^;]*; do rm -rf \$T/\$d; cp -r \$D/\$d", pipe) is not None, "pipeline syncs device/hisense/a6l/audio (incl. audio/bluetooth)")
check("audio/audio_policy_configuration.xml $T/audio/realinit/audio_policy_configuration.xml" in pipe, "pipeline installs this policy XML at the realinit path")

if L:
    pc = open(os.path.join(L, "system/sepolicy/private/property_contexts")).read()
    for k, v in props.items():
        m = re.search(r"^" + re.escape(k) + r"\s+u:object_r:(\w+):s0\s+exact\s+(\w+)", pc, re.M)
        ok = m is not None and (m.group(2) != "bool" or v in ("true", "false"))
        check(ok, f"property_contexts exact entry for {k}" + (f" ({m.group(1)} {m.group(2)})" if m else ""))
    vi = open(os.path.join(L, "system/sepolicy/private/vendor_init.te")).read()
    for t in ("bluetooth_config_prop", "bluetooth_a2dp_offload_prop"):
        check(f"set_prop(vendor_init, {t})" in vi, f"vendor_init may set {t}")
    xsd = os.path.join(L, "hardware/interfaces/audio/aidl/default/config/audioPolicy/audio_policy_configuration.xsd")
    if os.path.exists(xsd) and subprocess.run(["which", "xmllint"], capture_output=True).returncode == 0:
        txt = open(pol).read()
        txt = txt.replace('<xi:include href="audio_policy_volumes.xml"/>', '<volumes/>')  # stand-in for the included file
        txt = re.sub(r'<xi:include[^>]*/>', '', txt)
        # the AIDL xsd has no profile@name; the xsdc parser of the HAL ignores it (primary ports carry name="" since audio3)
        txt = re.sub(r'<profile name=""', '<profile', txt)
        with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False) as f:
            f.write(txt)
        r = subprocess.run(["xmllint", "--noout", "--schema", xsd, f.name], capture_output=True, text=True)
        check(r.returncode == 0, "policy XML validates against the tree's AIDL audio_policy_configuration.xsd (xi:include and profile@name stripped)")
        if r.returncode: print(r.stderr[-2000:])
        os.unlink(f.name)
    else:
        print("SKIP xsd validation (no xsd or xmllint)")
print(("BT_AUDIO_CHECK FAIL %d" % len(fails)) if fails else "BT_AUDIO_CHECK PASS")
sys.exit(1 if fails else 0)
PY
