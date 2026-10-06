# btcall kernel: in-call record + in-call music for the BT call bridge (29 Sep 2026)

Offline work. No phone, no adb, no `m`. **Never run on the phone.** The ROM default stays on **audio4**. The series set
below is opt-in (`A6L_AUDIO_SET=series`) until a call has been validated with it.

This is the kernel half of `docs/android-bt-audio-20260929.md` ("Kernel work still needed", items 1-4). The userspace
bridge was already done and is default off (`persist.vendor.a6l.btcall.bridge`).

## What the DSP does (stock msm-4.4 model)
- **Downlink to the CPU (in-call record DL).**
  - The CVS taps the Rx stream end with `VSS_IRECORD_CMD_START` (0x000112BE): rx_tap = STREAM_END, tx_tap = NONE, port_id =
    `VSS_IRECORD_PORT_ID_DEFAULT`, which means Rx goes to AFE pseudo port **0x8003** (and Tx would go to 0x8004).
  - An ADM COPP on 0x8003 feeds an ASM capture session: **MultiMedia3**, pcmC0D3c.
- **Uplink from the CPU (in-call music).**
  - The ASM playback session **MultiMedia4** (pcmC0D4p) goes through an ADM COPP to AFE pseudo port **0x8005**.
  - `VSS_IPLAYBACK_CMD_START` (0x000112BD, port 0x8005) makes the CVS mix that port into the uplink.
- **The pseudo ports.** They are configured with `AFE_PARAM_ID_PSEUDO_PORT_CONFIG` (0x00010219): 16 bit, linear, timing mode
  TIMER (the ADSP clocks them). Then they are started with `AFE_PORT_CMD_DEVICE_START` like any other port.
- **Downstream reference.** LineageOS `android_kernel_xiaomi_sdm660` lineage-18.1 (msm-4.4):
  - `q6voice.c`: `voice_cvs_start_record`, `voc_start_record`, `voice_cvs_start_playback`
  - `q6afe.c`: `afe_port_start` pseudo branch
  - `msm-dai-q6-v2.c`: `msm_dai_q6_psuedo_port_hw_params`, incall_record / voc_playback DAIs
  - `msm-pcm-routing-v2.c`: `voc_start_record` on route set, `voc_start_playback`
  - `apr_audio-v2.h` / `q6voice.h`: payloads
- **The voice call itself is unchanged**: MVM passive session "11C05000", dual control, CVP on LPI_MI2S_RX_0/TX_3.

## Patch: `device/hisense/a6l/kernel/kvoice/a6l-btcall-incall-v75.patch`
It is in `kernel/rom-v2/series`, after `audfix/a6l-q6adm-endpoint2-v75.patch`. It touches 17 files.

| Piece | Change |
|---|---|
| dt-bindings `qcom,q6dsp-lpass-ports.h` | `INCALL_RECORD_RX` 153, `INCALL_RECORD_TX` 154, `VOICE_PLAYBACK_TX` 155 |
| `sound/soc/qcom/common.h` | `LPASS_MAX_PORT` = `VOICE_PLAYBACK_TX + 1` (156) |
| `q6afe.c/.h` | Port ids 0x8003/0x8004/0x8005, `struct afe_param_id_pseudo_port_cfg` (16 bytes, msm-4.4 layout) in the port config union, `port_maps`, cfg_type `AFE_PARAM_ID_PSEUDO_PORT_CONFIG`, new `q6afe_pseudo_port_prepare()` |
| `q6dsp-lpass-ports.c/.h` | DAIs "Voice Downlink Capture" / "Voice Uplink Capture" / "Voice Farend Playback" (downstream names; 8/16/48 kHz, 1-2 ch, S16). New `q6pseudo_ops` in the config struct (q6apm memsets its cfg, so NULL there) |
| `q6afe-dai.c` | `q6pseudo_hw_params`, prepare case, `q6pseudo_ops`, AIF widgets `INCALL_RECORD_RX/TX` (out) and `VOICE_PLAYBACK_TX` (in), routes |
| `q6routing.c` | Every `MultiMediaN Mixer` gains `INCALL_RECORD_RX` and `INCALL_RECORD_TX`. New `VOICE_PLAYBACK_TX Audio Mixer` with MultiMedia1..8 (stock "Incall_Music Audio Mixer"). Same per-direction session rules |
| `q6cvs.c/.h` | CVS **passive** control session (`VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION` 0x00011140, same name as the MVM session, so the `mmode1_session` override applies too), `q6cvs_start/stop_record`, `q6cvs_start/stop_playback` |
| `q6mvm.c/.h` | Exports `q6mvm_get_session_name()` (path default + the volte3 override) |
| `q6voice.c/.h` | Per path, `rec_rx/rec_tx/play` requests (kept across calls, like downstream `rec_info` / `music_info`). While a call runs they are synced at once. Otherwise they are applied right after `MVM START` (downstream `voice_setup_vocproc`) and stopped before `MVM STOP` (`voice_destroy_vocproc`). A record mode change is STOP + START. The CVS session is created on first use and released with the path. DSP errors are logged and never fail the call |
| `q6voice-dai.c`, `qcom,q6voice.h` | Three "codec" DAIs: `VOICEMMODE1_INCALL_REC_DL` 2, `_REC_UL` 3, `_INCALL_MUSIC` 4. They are the codec side of the pseudo-port back ends: `.prepare` requests the record/music (it runs after the cpu DAI started the AFE port), `.shutdown` withdraws it. `q6voice_dai_open()` now leaves BE substreams alone, because a BE shares the MultiMedia FE runtime |
| `sm8250.c` | The BE fixup keeps 48 kHz and sets **mono** on the three pseudo ports (downstream: BE channels = FE channels; the HAL streams are mono) |

**Why codec DAIs.** In mainline, a BE dai-link needs a codec (`qcom_snd_parse_of`). The downstream trigger is "route set"
(`msm_pcm_routing_process_audio`), which would make q6routing depend on q6voice. With codec DAIs, DPCM starts and stops
the tap exactly when the FE is open and routed to the pseudo port, and there is no new module dependency. The only new
dependency is q6cvs -> q6mvm, and `audio.txt` already loads q6mvm before q6cvs.

**Module-set coupling.** `LPASS_MAX_PORT` sizes arrays in q6afe, q6afe-dai, q6adm, q6routing, snd-soc-sm8250 and
snd-soc-qcom-common, and `struct q6dsp_audio_port_dai_driver_config` changes between snd-q6dsp-common and q6afe-dai.
So **all 19 staged sound/soc/qcom modules are rebuilt together**. Never mix them with audio4.

## DT: `kvoice/dt/a6l-voice-speaker-btcall-onbase-v75.dtso`
A superset of `a6l-voice-speaker-onbase-v75.dtso`, used **instead** of it and only with the series modules. Merged link order:

| # | Link | Kind | Notes |
|---|---|---|---|
| 0 | MultiMedia1 | FE | pcmC0D0, HAL |
| 1 | MultiMedia2 | FE | |
| 2 | VoiceMMode1 | FE | pcmC0D2, q6voiced |
| 3 | **MultiMedia3** | FE | capture-only q6asm dai@2 |
| 4 | **MultiMedia4** | FE | playback-only q6asm dai@3 |
| 5 | **Incall Record DL** | BE | q6afedai 153 + q6voicedai 2 |
| 6 | Speaker Playback | BE | |
| 7 | **Incall Music** | BE | q6afedai 155 + q6voicedai 4 |
| 8 | Internal MI2S Playback | BE | |
| 9 | Internal MI2S Capture | BE | |

- The overlay writes numeric ids, because the DT build includes the unpatched baseline dt-bindings.
- `/chosen hisense,a6l-btcall = "btcall-v75"`.
- Outside `/sound`, the q6asm dais and `/chosen`, the merged tree equals the default voice overlay (host test 4).
- With it, the btcall props are `persist.vendor.a6l.btcall.dl_pcm=3` and `ul_pcm=4`. The a6l-q6voiced default controls already
  match the kernel names: `MultiMedia3 Mixer INCALL_RECORD_RX` and `VOICE_PLAYBACK_TX Audio Mixer MultiMedia4`.

## Builds: `firmware/extracted/btcall-20260929/{v67,r5}` (+ `SHA256SUMS`, `build-info.txt`)
- **Build script:** `tools/build-btcall-series-modules.sh` (WSL, about 5 min). It works in three steps:
  - It runs the series check.
  - It copies the series `sound/soc/qcom` and builds it W=1 against `out-a6l-phone-v67` and against `out-a6l-rom-r5`
    (headers from `a6l-rom-r5-src`).
  - It keeps the 19 modules the ROM stages and strips their debug info.
- **Header order.** `NOSTDINC_FLAGS="-nostdinc -I<series dt-bindings>"` puts the series `qcom,q6dsp-lpass-ports.h` ahead of
  the tree's copy. `tools/check-rom-v2-kernel-series.sh` does the same.
- **Modules (19):** q6adm, q6afe, q6afe-clocks, q6afe-dai, q6asm, q6asm-dai, q6core, q6cvp, q6cvs, q6mvm, q6routing,
  q6voice, q6voice-common, q6voice-dai, snd-q6dsp-common, snd-soc-qcom-common, snd-soc-qcom-offload-utils,
  snd-soc-qcom-sdw, snd-soc-sm8250. They carry the full series: volte3 session select, F6 TX Mute, F37 RX Volume, SSR
  fix, per-direction q6routing, ADM endpoint2, btcall.
- **Verification:**
  - Both builds: 0 W=1 warnings.
  - V67: every undefined symbol resolves (System.map, in-tree Module.symvers, the set).
  - r5: exact modversions vermagic. Every import of the set, and every import that another staged r5 module makes from
    the set (e.g. q6routing-upstream), matches the export CRC.
  - Result: `BTCALL_SERIES_VERIFY PASS`, `BTCALL_SERIES_BUILD_PASS`.

## Using it (opt-in)
```
A6L_AUDIO_SET=series [A6L_KERNEL=r5] bash tools/rom-v2-pipeline.sh     # DT + stage both follow the variable
```
- `tools/build-rom-v2-dt.sh`: `A6L_AUDIO_SET=series` swaps in the btcall voice overlay.
- `tools/stage-rom-v2-prebuilts.sh`:
  - `A6L_AUDIO_SET=series` overrides the 19 modules with `btcall-20260929/$A6L_KERNEL` (checked against SHA256SUMS). This
    happens after the r5 replacement.
  - Every name must already be in the staged set.
  - `audio.txt` is unchanged.
- Default (unset = `audio4`): DTB and staged set byte-identical to before.

## Tests (all offline, PASS)
- `device/hisense/a6l/kernel/kvoice/tests/run-btcall-tests.sh` -> `BTCALL_KERNEL_TESTS PASS`. It rebuilds the q6voice
  sources from the series patches (no kernel tree needed) and covers four areas:
  1. **`test_btcall.c` (ASan/UBSan), 119 checks.** The real q6voice.c + q6cvs.c run against stubbed MVM/CVP/APR. The test
     covers:
     - APR payload sizes and fields (msm-4.4)
     - the passive session name, including the override
     - no CVS traffic without a request
     - a request before a call is applied after MVM START
     - STOPs happen before MVM STOP
     - requests resume on the next call
     - rx -> rx+tx is STOP + START
     - withdraw
     - DSP errors do not fail the call and are retried
     - an invalid path
  2. The `AFE_PARAM_ID_PSEUDO_PORT_CONFIG` payload is 16 bytes with the msm-4.4 offsets, and the AFE ids are right.
  3. The ids agree across dt-bindings, q6voice-dai, the DT overlay and the a6l-q6voiced default control names.
  4. **DT merge on the V74 base.** It checks the link order above, then `check-voice-dt-links.py` PASS (voice pcm 2),
     `check-audio-dt-links.py --rule reg --expect-tfa` PASS, and that nothing else changed.
- `tools/check-rom-v2-kernel-series.sh`: `A6L_KSERIES_PASS`. It now also builds and checks q6afe, q6afe-dai,
  snd-q6dsp-common and q6adm, plus the btcall markers.
- The stage dry runs (default v67 and r5, plus `A6L_AUDIO_SET=series` for both) and the ROM static tests: see the ledger line.

## Attended (first call with the series set = also the first bridge try)
The ROM or boot image must be built with `A6L_AUDIO_SET=series`. The test order is:
1. **Series set, bridge off.**
   - `dmesg`: sound card registered, no `Invalid cpu dai id`, no q6afe `Invalid port id`.
   - `cat /proc/asound/pcm`: 00 MultiMedia1, 02 VoiceMMode1, 03 MultiMedia3 (capture), 04 MultiMedia4 (playback).
   - A call on earpiece / headset / speaker works as with audio4.
   - Check F6 (Android mute: the far end hears silence and it is restored) and F37 (in-call volume steps).
   - `tinymix | grep -i "INCALL\|VOICE_PLAYBACK"` lists the new controls.
2. **Bridge.** Follow `docs/android-bt-audio-20260929.md` test 7 with `persist.vendor.a6l.btcall.dl_pcm 3` and `ul_pcm 4`. Watch:
   - dmesg `A6L_Q6VOICE cvs passive session '11C05000'`
   - `incall record start rx=1 tx=0: 0`
   - `incall music start: 0`
   - on the far end: headset mic yes, phone mic no
3. **If it fails, note which step:**
   - AFE pseudo-port start errors (the DSP refuses 48 kHz mono). Try it by hand: `tinycap -D 0 -d 3 -r 8000 -c 1` while
     `MultiMedia3 Mixer INCALL_RECORD_RX` = 1.
   - CVS passive create errors: check the session name (`q6mvm.mmode1_session`).
   - `VSS_IRECORD` status.
   - Whether the CVP TX mute silences in-call music. If it does, use `persist.vendor.a6l.btcall.cvp_mute=0`.
4. **Rollback:** a ROM built without `A6L_AUDIO_SET` (audio4 + default DT).

**Open risks.** The pseudo-port rate/channels (48 kHz mono) and the CVS passive session on this CVD firmware have not been
proven. Downstream used the same commands on this ADSP, but the stock HAL may have used other rates. The UL record
back end (port 154) has no DT link yet: the bridge does not need it.
