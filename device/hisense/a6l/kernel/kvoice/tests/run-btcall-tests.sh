#!/usr/bin/env bash
# btcall (29 Sep 2026): host tests of kvoice/a6l-btcall-incall-v75.patch (in-call record / in-call music for the
# cellular-call-to-Bluetooth bridge). No kernel tree needed: the q6voice files are rebuilt from the kernel series
# patches in rom-v2/series order (the full series creates them; volte3, F6, F37, ssr and btcall modify them).
#  1. test_btcall.c (ASan/UBSan): q6voice.c + q6cvs.c state machine and APR payloads (msm-4.4 voice.c parity)
#  2. q6afe AFE_PARAM_ID_PSEUDO_PORT_CONFIG payload layout (msm-4.4 struct afe_param_id_pseudo_port_cfg)
#  3. id consistency: dt-bindings (153/154/155, LPASS_MAX_PORT), q6voice-dai ids 2/3/4, DT overlay, q6voiced default ctls
#  4. DT overlay: merged on the V74 base, link order + check-voice-dt-links.py + check-audio-dt-links.py (needs
#     dtc/fdtoverlay + firmware/extracted/recovery-v74-candidate-20260923/base.dtb; skipped with a note otherwise)
# usage: run-btcall-tests.sh   -> BTCALL_KERNEL_TESTS PASS|FAIL
set -uo pipefail
H=$(cd "$(dirname "$0")" && pwd); KV=$H/..; R=$(cd $KV/../../../../.. && pwd); B=$(mktemp -d); trap 'rm -rf $B' EXIT
fail=0; ok() { echo "  ok   $*"; }; bad() { echo "  FAIL $*"; fail=1; }
Q=$B/src/sound/soc/qcom/qdsp6; mkdir -p $Q
# --- sources: new q6voice files from the full patch, then every later kvoice patch hunk for those files ---
python3 - "$KV" "$B/src" <<'PY'
import os, re, subprocess, sys
kv, root = sys.argv[1], sys.argv[2]
def sections(path):
    txt = open(path, encoding='utf-8').read().replace('\r', '')
    out = []
    for p in re.split(r'(?m)^(?=diff --git )|^(?=--- a/)', txt):
        m = re.search(r'(?m)^\+\+\+ b/(\S+)', p)
        if m: out.append((m.group(1), p))
    return out
want = ('q6voice.c', 'q6voice.h', 'q6cvs.c', 'q6cvs.h', 'q6mvm.h', 'q6cvp.h', 'q6voice-common.h')
for f, p in sections(os.path.join(kv, 'a6l-q6voice-full-7.2.3.patch')):
    if os.path.basename(f) in want and '--- /dev/null' in p:
        body = [l[1:] for l in p.split('\n@@', 1)[1].split('\n')[1:] if l[:1] in ('+', ' ')]
        os.makedirs(os.path.join(root, os.path.dirname(f)), exist_ok=True)
        open(os.path.join(root, f), 'w').write('\n'.join(body) + '\n')
for name in ('a6l-q6mvm-volte-session-v75.patch', 'a6l-q6voice-tx-mute-v75.patch', 'a6l-q6voice-rx-volume-v75.patch',
             'a6l-q6voice-ssr-v75.patch', 'a6l-btcall-incall-v75.patch'):
    sel = [p for f, p in sections(os.path.join(kv, name)) if os.path.exists(os.path.join(root, f))]
    if not sel: continue
    r = subprocess.run(['patch', '-s', '-p1', '-d', root], input=''.join(sel), text=True, capture_output=True)
    if r.returncode: sys.exit(f'patch {name}: {r.stdout}{r.stderr}')
PY
[ $? = 0 ] || { echo "BTCALL_KERNEL_TESTS FAIL (sources)"; exit 1; }
grep -q q6voice_set_incall_record $Q/q6voice.c && grep -q tx_mute $Q/q6voice.c && grep -q rx_vol_step $Q/q6voice.c \
  && grep -q VSS_IRECORD_CMD_START $Q/q6cvs.c && ok "sources rebuilt from the series (F6 + F37 + btcall present)" || bad "sources"
echo "== 1. q6voice/q6cvs state machine (ASan/UBSan)"
if gcc -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Werror -Wno-unused-function \
     -Wno-address-of-packed-member -I$H/stub-btcall -I$Q -o $B/test_btcall $H/test_btcall.c 2> $B/cc.log; then
  out=$(timeout 30 $B/test_btcall 2>&1); echo "$out" | tail -3 | sed 's/^/  /'
  echo "$out" | grep -q "BTCALL_Q6VOICE_TEST PASS" && ok "test_btcall" || bad "test_btcall"
else head -20 $B/cc.log; bad "test_btcall build"; fi
P=$KV/a6l-btcall-incall-v75.patch
echo "== 2. AFE pseudo port payload layout"
awk '/^\+struct afe_param_id_pseudo_port_cfg \{/{on=1} on{print substr($0,2)} on&&/^\+\} __packed;/{exit}' <(tr -d '\r' < $P) > $B/pcfg.h
cat > $B/pcfg.c <<'C'
#include <stddef.h>
#include <stdint.h>
typedef uint16_t u16; typedef uint32_t u32;
#define __packed __attribute__((packed))
#include "pcfg.h"
/* msm-4.4 apr_audio-v2.h: minor_version u32, bit_width u16, num_channels u16, data_format u16, timing_mode u16, sample_rate u32 */
_Static_assert(sizeof(struct afe_param_id_pseudo_port_cfg) == 16, "size");
_Static_assert(offsetof(struct afe_param_id_pseudo_port_cfg, bit_width) == 4, "bit_width");
_Static_assert(offsetof(struct afe_param_id_pseudo_port_cfg, num_channels) == 6, "num_channels");
_Static_assert(offsetof(struct afe_param_id_pseudo_port_cfg, data_format) == 8, "data_format");
_Static_assert(offsetof(struct afe_param_id_pseudo_port_cfg, timing_mode) == 10, "timing_mode");
_Static_assert(offsetof(struct afe_param_id_pseudo_port_cfg, sample_rate) == 12, "sample_rate");
int main(void) { return 0; }
C
gcc -I$B -o $B/pcfg $B/pcfg.c && ok "afe_param_id_pseudo_port_cfg 16 bytes, msm-4.4 offsets" || bad "pseudo port cfg layout"
tr -d '\r' < $P > $B/p.patch; P=$B/p.patch
grep -q '^+#define AFE_PARAM_ID_PSEUDO_PORT_CONFIG	0x00010219' $P && grep -q '^+#define AFE_PORT_ID_VOICE_RECORD_RX	0x8003' $P \
  && grep -q '^+#define AFE_PORT_ID_VOICE_RECORD_TX	0x8004' $P && grep -q '^+#define AFE_PORT_ID_VOICE_PLAYBACK_TX	0x8005' $P \
  && grep -q '^+#define AFE_PSEUDOPORT_TIMING_MODE_TIMER	0x1' $P && ok "AFE ids 0x10219 / 0x8003 / 0x8004 / 0x8005, timer mode" || bad "AFE ids"
echo "== 3. id consistency"
grep -q '^+#define INCALL_RECORD_RX	153' $P && grep -q '^+#define INCALL_RECORD_TX	154' $P && grep -q '^+#define VOICE_PLAYBACK_TX	155' $P \
  && grep -q '^+#define LPASS_MAX_PORT			(VOICE_PLAYBACK_TX + 1)' $P && ok "dt-bindings 153/154/155, LPASS_MAX_PORT 156" || bad "dt-bindings ids"
grep -q '^+#define	VOICEMMODE1_INCALL_REC_DL	2' $P && grep -q '^+#define	VOICEMMODE1_INCALL_MUSIC	4' $P && ok "q6voice-dai incall ids 2/4" || bad "q6voice ids"
D=$KV/dt/a6l-voice-speaker-btcall-onbase-v75.dtso
grep -q 'sound-dai = <&q6afedai 153>' $D && grep -q 'sound-dai = <&q6afedai 155>' $D && grep -q 'sound-dai = <&q6voicedai 2>' $D \
  && grep -q 'sound-dai = <&q6voicedai 4>' $D && ok "DT overlay uses the same ids" || bad "DT overlay ids"
QD=$R/device/hisense/a6l/kvoice/q6voiced/a6l_q6voiced.c
if [ -f $QD ]; then
  grep -q 'MultiMedia3 Mixer INCALL_RECORD_RX' $QD && grep -q 'VOICE_PLAYBACK_TX Audio Mixer MultiMedia4' $QD \
    && grep -q 'SOC_SINGLE_EXT("INCALL_RECORD_RX", INCALL_RECORD_RX' $P && grep -q 'SND_SOC_DAPM_MIXER("VOICE_PLAYBACK_TX Audio Mixer"' $P \
    && ok "a6l-q6voiced default dl_ctl/ul_ctl = the q6routing control names" || bad "q6voiced ctl names"
else echo "  skip q6voiced ctl names ($QD not found)"; fi
echo "== 4. DT overlay merge"
BASE=$R/firmware/extracted/recovery-v74-candidate-20260923/base.dtb
DTC=${DTC:-$(command -v dtc || true)}
if [ -n "$DTC" ] && command -v fdtoverlay > /dev/null && [ -f $BASE ]; then
  for n in a6l-voice-speaker-onbase-v75 a6l-voice-speaker-btcall-onbase-v75; do
    tr -d '\r' < $KV/dt/$n.dtso > $B/$n.dtso
    cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp $B/$n.dtso -o $B/$n.pp && $DTC -@ -q -I dts -O dtb -o $B/$n.dtbo $B/$n.pp \
      && fdtoverlay -i $BASE -o $B/$n.dtb $B/$n.dtbo || bad "merge $n"
  done
  v=$(python3 $R/tools/check-voice-dt-links.py $B/a6l-voice-speaker-btcall-onbase-v75.dtb); echo "$v" | grep '^link' | sed 's/^/  /'
  echo "$v" | grep -q 'A6L_VOICE_DT_CHECK PASS voice_pcm_device=2' && ok "check-voice-dt-links PASS, voice pcm 2" || bad "check-voice-dt-links"
  python3 $R/tools/check-audio-dt-links.py $B/a6l-voice-speaker-btcall-onbase-v75.dtb --rule reg --expect-tfa | grep -q 'A6L_DT_CHECK PASS' \
    && ok "check-audio-dt-links --rule reg --expect-tfa PASS" || bad "check-audio-dt-links"
  names=$(echo "$v" | grep '^link' | sed -E "s/^link ([0-9]+): '([^']*)'.*/\1=\2/" | tr '\n' ',')
  [ "$names" = "0=MultiMedia1,1=MultiMedia2,2=VoiceMMode1,3=MultiMedia3,4=MultiMedia4,5=Incall Record DL,6=Speaker Playback,7=Incall Music,8=Internal MI2S Playback,9=Internal MI2S Capture," ] \
    && ok "link order: MM3 = pcm 3 (dl_pcm), MM4 = pcm 4 (ul_pcm)" || bad "link order $names"
  # outside /sound, the q6asm dais and /chosen the merged tree equals the default voice overlay
  $DTC -q -I dtb -O dts $B/a6l-voice-speaker-onbase-v75.dtb > $B/a.dts; $DTC -q -I dtb -O dts $B/a6l-voice-speaker-btcall-onbase-v75.dtb > $B/b.dts
  python3 - $B/a.dts $B/b.dts <<'PY' && ok "no change outside /sound, q6asm dais, /chosen (+ symbol tables)" || bad "unexpected DT diff"
import re, sys
def nodes(p):
    out, stack = {}, []
    for l in open(p):
        s = l.strip()
        m = re.match(r'(\S+) \{$', s)
        if m: stack.append(m.group(1)); continue
        if s == '};': stack.pop(); continue
        if s and stack: out.setdefault('/'.join(stack), []).append(re.sub(r'<0x[0-9a-f]+', '<PH', s))
    return out
a, b = nodes(sys.argv[1]), nodes(sys.argv[2])
diff = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
unexpected = [k for k in diff if not re.search(r'/sound|service@7/dais|/chosen|__symbols__|__local_fixups__|__fixups__', k)]
print('  changed nodes:', len(diff), 'unexpected:', unexpected)
sys.exit(1 if unexpected else 0)
PY
else echo "  skip DT merge (dtc/fdtoverlay or $BASE missing)"; fi
echo "BTCALL_KERNEL_TESTS $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
