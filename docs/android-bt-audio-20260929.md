# Android side: Bluetooth audio (A2DP + HFP/SCO), 29 Sep 2026

Prepared offline for the r5 ROM. No phone, no adb, nothing built with `m`. Target: the rom-v2 product `lineage_gsi_a6l`, which inherits `rom/rom.mk`.

## What was missing
* **No Bluetooth profile would start.** In AOSP, A2dpService, HeadsetService and the other profiles check `BluetoothProperties.isProfile*Enabled().orElse(false)`. Neither the GSI system image nor our vendor set any `bluetooth.profile.*`. The r4 ROM could turn BT on and scan, but it could not play music or connect a headset for calls.
* **No SCO ports** existed in the audio policy, and SCO had no data path. The mainline q6 stack has no downstream btfm slimbus link from WCN3990 to the ADSP, so SoC PCM/slimbus SCO is not available. The only usable path is HCI SCO over the UART. Cellular calls reach the headset only through the btcall bridge (below, default off).

## What is ready
| Item | Where |
|---|---|
| Profiles enabled: A2DP source, AVRCP target, HFP AG, GATT, HID host/device, MAP, OPP, PAN, PBAP. LE audio profiles (BAP/BASS/CSIP/VCP/HAP/CCP/MCP), ASHA and offload are disabled (`ro.bluetooth.a2dp_offload.supported=false`, `persist.bluetooth.a2dp_offload.disabled=true`, `persist.bluetooth.leaudio_offload.disabled=true`). Class of device is set to phone (`90,2,12`). | `device/hisense/a6l/audio/bluetooth/bt-audio.mk` (PRODUCT_VENDOR_PROPERTIES) |
| HFP software data path: `bluetooth.sco.managed_by_audio=true` (the framework flag `android.media.audio.sco_managed_by_audio` is ENABLED in cp2a) and `bluetooth.hfp.software_datapath.enabled=true`. The stack sets up eSCO with data path 0 (HCI). The controller does CVSD transcoding; mSBC and LC3-SWB go through transparent mode. | same |
| Wiring: `$(call inherit-product, device/hisense/a6l/audio/bluetooth/bt-audio.mk)` added after the Wi-Fi/BT block. `audio/` is synced by `tools/rom-v2-pipeline.sh` prep. | `rom/rom.mk` |
| SCO ports in the existing `bluetooth` module: dynamic mix ports `hfp output` and `hfp input`; devices `BT SCO`, `BT SCO Headset`, `BT SCO Car Kit` and `BT SCO Headset Mic` (8/16/32 kHz, mono, 16 bit, the HfpSoftwareAudioProvider limits); routes added. The A2DP part (`a2dp output`, the A2DP devices) was already there. In the AIDL example HAL, ModuleBluetooth creates the IBluetoothAudioProviderFactory in-process (`android.hardware.bluetooth.audio-impl`, declared by the APEX VINTF). SCO ports also make `IModule.getBluetooth()` non-null. | `audio/audio_policy_configuration.xml` |
| sepolicy: no change needed. The AOSP policy already covers it: provider service = `hal_audio_service` (served by hal_audio_default), `hal_client_domain(bluetooth, hal_audio)`, and vendor_init may set `bluetooth_config_prop` / `bluetooth_a2dp_offload_prop`. Every property has an exact `property_contexts` entry of the right type (checked). | - |
| Test: `audio/bluetooth/tests/check-bt-audio.sh [lineage-tree]` covers the XML structure, the properties, the product wiring, property_contexts types, and XSD validation against the tree's AIDL `audio_policy_configuration.xsd`. It gives 71/71 PASS with the tree. `rom/tests/test-rom-static.sh` PASS. | - |

## Cellular calls on a BT headset: the "btcall" bridge (29 Sep 2026, offline, default OFF)
Until now voice ran only on the ADSP (modem to CVD to AFE, driven by a6l-q6voiced), and the CPU never saw the call PCM. The primary module had no Telephony Rx/Tx devices, so when a call was routed to BT the APM fell back to legacy primary-output routing. The primary module cannot reach SCO, so the call audio stayed on the earpiece/speaker/jack. HFP call control, VoIP/communication audio and the SCO mic already worked over BT.

### How stock does it (SDM660 + WCN3990): hardware path over SLIMbus
Evidence: `firmware/extracted/stock-dtbo-20260914/stock-00-merged.dts`, `stock-assets/audio/mixer_paths.xml`, `stock/kernel.config`, `stock.kallsyms`.
* WCN3990 has a SLIMbus port on the **second NGD controller** `slim@15240000` (cell-index 3, `qcom,slim-ngd`), child `wcn3990 { compatible = "qcom,btfmslim_slave"; elemental-addr = [00 01 20 02 17 02]; }`. The kernel has `CONFIG_BTFM_SLIM(_WCN3990)=y` and `CONFIG_SLIMBUS_MSM_NGD=y` (`btfm_slim_*` in kallsyms).
* The ADSP sees BT SCO as AFE ports **SLIMBUS_7_RX/TX = 0x400e/0x400f** (dai-links `msm-dai-q6-dev.16398/16399`). The call goes DSP-to-DSP:
  * `SLIM_7_RX_Voice Mixer VoiceMMode1`
  * `VoiceMMode1_Tx Mixer SLIM_7_TX_MMode1`
  * media: `SLIMBUS_7_RX Audio Mixer MultiMedia*`

  The stock BT stack keeps SCO off HCI (vendor data path = SLIMbus).
* The stock ADSP firmware also has the CPU-access ports that the fallback needs:
  * in-call record RX/TX (0x8003/0x8004): `MultiMedia1 Mixer VOC_REC_DL/UL`
  * in-call music (0x8005, `Incall_Music Audio Mixer MultiMedia2`) and music-2 (0x8002)
  * AFE RT-proxy (`AFE_PCM_RX/TX`, `afe_rt_proxy_port_read/write`)
  * voice host-PCM (`hpcm_*`)

### What exists on our mainline q6 stack (7.2.3 r5 tree)
| Piece | Status |
|---|---|
| q6afe SLIMBUS ports | 0..6 only (0x4000..0x400d); **no SLIMBUS_7** (would be about 20 lines) |
| `qcom-ngd-ctrl` SLIMbus controller | upstream (SDM845/wcd934x), but **no DT node** for SDM660's second NGD, and it has never been run on SDM660 |
| WCN3990 btfm SLIMbus codec driver | **absent upstream** (downstream `btfm_slim*` is about 1.5k lines, plus WCN3990 vendor commands to route SCO to SLIMbus). It would also conflict with our HCI SCO software data path (bt-audio). |
| in-call record / in-call music / RT-proxy / host-PCM AFE ports | **absent** in q6afe/q6afe-dai/q6routing; q6voice has no CVS record/playback start |
| userspace | AOSP AudioFlinger can bridge by itself ("software bridge" between two HAL modules); our HFP software data path already carries SCO PCM between the audio HAL and the stack |

### Options considered
1. **Stock hardware path (SLIMbus 7)**: port btfm_slim, add an SDM660 NGD2 DT node and SLIMBUS_7 AFE ports, then switch the BT stack to the vendor SCO data path.
   * Needs 3 new kernel pieces on an untested bus, plus BT vendor commands, and it breaks the working HCI SCO path.
   * Not feasible offline and very risky. Rejected for now.
2. **AFE RT-proxy or voice host-PCM**: route the voice session to proxy ports (or tap the CVS), and the CPU reads/writes shared memory.
   * Clean on the DSP side, but it needs a new PCM driver with the APR shared-memory data commands.
   * Proxy ports have no hardware clock, and the voice session timing then depends on the CPU.
3. **Chosen: CPU bridge through AudioFlinger, with the DSP side on in-call record + in-call music.**
   * The voice session stays exactly as it is today. The earpiece LPI_MI2S port keeps clocking CVP, and the codec is silenced.
   * The CPU gets the downlink from **in-call record DL** (AFE pseudo-port 0x8003 to an ASM capture front end).
   * It returns the headset mic through **in-call music** (ASM playback front end to pseudo-port 0x8005, mixed into the uplink by the CVS). The phone mic is muted in the CVP ("VoiceMMode1 TX Mute").
   * AudioFlinger does the BT side with the standard **software call bridge**, because the policy now has Telephony Rx/Tx devices: Telephony Rx -> record thread -> `hfp output` -> BT SCO, and BT SCO mic -> `hfp input` -> playback thread -> Telephony Tx. It uses our existing HFP software data path.
   * The kernel pieces are small, well-known pseudo ports that the stock ADSP firmware already has (stock mixer_paths use them). The userspace can be built and tested offline now.
   * The a6l-q6voiced daemon alone cannot do the bridge, because the BT stack owns the SCO PCM through the audio HAL's BluetoothAudio session and a vendor daemon cannot reach it. The daemon owns the DSP side instead.

### Implemented (offline, all default OFF)
| Piece | Where | What |
|---|---|---|
| Bridge policy | `audio/bluetooth/btcall/audio_policy_configuration_btcall.xml`, generated from the product policy by `gen-btcall-policy.py` (`--check` catches drift) | Primary module + attached `Telephony Tx`/`Telephony Rx`, mix ports `telephony tx`/`telephony rx` (8/16 kHz mono). HW routes Telephony Rx -> Earpiece/Speaker/Wired, phone mics -> Telephony Tx: calls on the phone become HW patches that the 0002 publisher already reports. Telephony Rx -> `telephony rx` and `telephony tx` -> Telephony Tx: the only way to reach BT SCO, so the APM builds the SW bridge. Other modules unchanged. |
| Activation | `audio/bluetooth/btcall/init.a6l.btcall.rc` | `on early-boot && property:persist.vendor.a6l.btcall.bridge=1`: bind-mounts the bridge policy over `/vendor/etc/audio_policy_configuration.xml` (read by both the HAL and audioserver). Needs a reboot. |
| Wiring | `audio/bluetooth/bt-audio.mk` | PRODUCT_COPY_FILES of both files. No property is set: default off. |
| Audio HAL | `audio/patches/0004-a6l-btcall-bridge.patch` (StreamPrimary, AOSP lines only, so 0001..0004 all still reverse-apply for the pipeline) | With `persist.vendor.a6l.btcall.bridge=1`, the Telephony Rx input and Telephony Tx output streams open `pcmC0D<dl_pcm>c` / `pcmC0D<ul_pcm>p` (props `persist.vendor.a6l.btcall.dl_pcm` / `ul_pcm`, 1..31) instead of the AOSP stub driver. |
| a6l-q6voiced bridge mode | `kvoice/q6voiced/a6l_q6voiced.c` (`-b <prefix>`, default `persist.vendor.a6l.btcall.`, read every loop) | See the list below this table. |
| a6l-audio-route | `audio/route/a6l_audio_route.c`, `audio/mixer_paths_a6l.xml` | Same bridge check: in a call -> `voice-bridge` / `bridge-mic` (EAR_S/HPHL/HPHR ZERO, DEC1 MUX ZERO): nothing is audible from the phone and no phone mic reaches the uplink, even with a wired headset plugged in. |
| sepolicy | `audio/sepolicy/property_contexts` (+`persist.vendor.a6l.btcall.` -> vendor_a6l_audio_prop), `audio/sepolicy/btcall.te` (`allow init vendor_configs_file:file mounton`) | The readers already have the needed rules: the HAL through set_prop, both daemons through get_prop + proc_asound. `check-a6l-sepolicy.sh` userdebug and user+rom/sepolicy/vendor: PASS. |
| Tests | `audio/bluetooth/btcall/tests/check-btcall.sh [tree]` | Policy generation/structure/XSD, init/product/sepolicy wiring, default off, patch series on the pristine tree HEAD in pipeline order + reverse-apply, q6voiced host tests (130 PASS: 41 new btcall), audio-route tests (258 PASS incl. the real libaudioroute on the new paths) + publish tests 12 PASS -> `BTCALL_CHECK PASS`. |

a6l-q6voiced bridge mode does the following:
* While the bridge is enabled (even between calls, so the HAL's streams never open a front end without a back end), it connects the ADM routes `dl_ctl` (default `MultiMedia3 Mixer INCALL_RECORD_RX`) and `ul_ctl` (default `VOICE_PLAYBACK_TX Audio Mixer MultiMedia4`).
  * Retries are 5 s apart, then 60 s after 3 failures, so there is no log storm on a kernel without them.
  * The routes are cleared when the bridge is disabled, and renamed routes are moved.
  * They are re-asserted at every call start and after a card loss.
* A call is **bridged** while it is active and the HAL has the downlink front end open: `/proc/asound/card0/pcm<dl_pcm>c/sub0/status` is not "closed". No new IPC, and no change to the 0002 publisher is needed. While a call is bridged:
  * the phone mic is muted in the DSP (unless `cvp_mute=0`)
  * the voice RX stays on the earpiece port (DSP clock) whatever `call_out` says
  * Android's mute also disconnects the uplink injection route. A CVP mute would not silence in-call music. A failure is reported as `ERR`.
  * `status` appends `bridge=0|1`

### Kernel work (DONE offline 29 Sep 2026, opt-in: docs/btcall-kernel-20260929.md)
**Update:** items 1-4 below are implemented in `kernel/kvoice/a6l-btcall-incall-v75.patch` (rom-v2 series), with the DT in
`kvoice/dt/a6l-voice-speaker-btcall-onbase-v75.dtso` and the module set in `firmware/extracted/btcall-20260929/{v67,r5}`.
They are selected together by `A6L_AUDIO_SET=series` (stage + DT; the default ROM stays on audio4). With them:
MultiMedia3 = pcmC0D3c (`dl_pcm=3`), MultiMedia4 = pcmC0D4p (`ul_pcm=4`); the control names equal the q6voiced defaults.
The design notes below are the original plan.
1. **q6afe**:
   * pseudo ports `VOICE_RECORD_RX` 0x8003, `VOICE_RECORD_TX` 0x8004, `VOICE_PLAYBACK_TX` 0x8005 (and 0x8002)
   * `AFE_PARAM_ID_PSEUDO_PORT_CONFIG` (downstream `afe_port_start` pseudo branch: bit width 16, 1 ch, data format 0, timing mode 1 = timer)
   * DAIs in q6afe-dai / q6dsp-lpass-ports (dt-bindings ids after LPI_MI2S_TX_6)
2. **q6routing**:
   * `MultiMediaN Mixer INCALL_RECORD_RX|TX` (capture: ADM COPP on the pseudo port)
   * `VOICE_PLAYBACK_TX Audio Mixer MultiMediaN` (playback)
   * same rules as the existing per-direction ADM maps (`persist.vendor.a6l.audio.q6routing=perdir`)
3. **q6voice (our kvoice series)**:
   * when the pseudo-port BE starts and the MVM/CVS session is running (or at session start if the BE is already up), send `VSS_IRECORD_CMD_START` (0x000112BE; rx_tap_point = STREAM, tx_tap_point = NONE, port_id 0x8003, mode = VSS_IRECORD_MODE_TX_RX_STEREO or MIXING) and `VSS_IPLAYBACK_CMD_START` (0x000112BD; port_id 0x8005) to the CVS
   * send `..._STOP` at stop and at session end
   * the downstream reference is `voc_start_record` / `voc_start_playback` in `voice.c` (both in stock kallsyms)
4. **DT**: two more DPCM front ends (e.g. MultiMedia3 capture, MultiMedia4 playback) and BE dai-links for the pseudo ports in the A6L sound node. Then set `persist.vendor.a6l.btcall.dl_pcm/ul_pcm` to their pcm numbers (from `/proc/asound/pcm`), and `dl_ctl`/`ul_ctl` if the control names differ.
5. Open question for the attended test: does the CVP TX mute leave in-call music audible? The downstream design suggests yes: device mute is in VPTX, and playback is mixed in the stream. If not, use `persist.vendor.a6l.btcall.cvp_mute=0`, because the codec input path is already off (`bridge-mic`).

Without these kernel pieces, enabling the bridge gives this: a BT-routed call builds the SW bridge, the HAL fails to open the missing PCM, and the headset stays silent. Calls on the phone (HW patches) keep working. Keep it OFF until the kernel exists.

## Attended tests after the first install
1. `getprop | grep bluetooth.profile` shows the values above. `dumpsys bluetooth_manager` lists A2dpService and HeadsetService as started, with no LeAudioService.
2. `dumpsys media.audio_flinger` / `dumpsys media.audio_policy`: the module `bluetooth` loads, and the ports include `hfp output`/`hfp input`. `logcat | grep -i "BluetoothAudio\|ModuleBluetooth"` must not show "IBluetoothAudioProviderFactory AIDL service not available".
3. **A2DP**: pair a headset or speaker and play music. Check that the codec shows as SBC/AAC (Developer options), with no stutter under screen-off and e-ink use. `logcat -s bluetooth` should show `A2DP_SOFTWARE_ENCODING_DATAPATH`.
4. **SCO/HCI** (key unknown: does WCN3990 firmware accept eSCO data path HCI over UART?):
   * run a VoIP call or the Sound Recorder with a BT mic
   * or use `adb shell cmd audio set-communication-device` (or a test app calling `AudioManager.setCommunicationDevice(TYPE_BLUETOOTH_SCO)`)
   * check `logcat`: `HFP_SOFTWARE_ENCODING_DATAPATH` session started, `btm_sco` connected with `input_data_path=HCI` and no fallback loop
   * listen for two-way audio (CVSD and mSBC headsets)
   * if eSCO fails: retry with `setprop bluetooth.sco.disable_enhanced_connection true` (legacy setup: the controller's default routing, probably PCM, i.e. silence)
   * capture `btsnoop` (`persist.bluetooth.btsnoopenable=true`)
5. Cellular call with a headset connected (bridge OFF, the default): answer/hang up from the headset works, and audio stays on the phone (expected without btcall). Make sure the call is not muted or broken when BT is selected in the call UI.
6. HID: pair a BT keyboard (uhid.ko is loaded by the base group, B2).
7. **btcall bridge** (needs a ROM built with `A6L_AUDIO_SET=series`: series modules + btcall DT; with the default audio4 ROM only
   the "default off" steps 7a/7b). The FIRST call test with the series set runs the normal call checks first (docs/btcall-kernel-20260929.md
   "Attended" step 1), then this test with `dl_pcm=3` / `ul_pcm=4`.
   * a. Default: `getprop | grep btcall` is empty. `mount | grep audio_policy` shows nothing. `dumpsys media.audio_policy` has no Telephony devices. `a6l-q6voiced` status (`echo status | nc -U /dev/socket/a6l_q6voiced` as root) has no `bridge=` field. A cellular call behaves as in test 5.
   * b. `setprop persist.vendor.a6l.btcall.bridge 1`, reboot:
     * `mount | grep audio_policy_configuration` shows the bind mount
     * `dumpsys media.audio_policy` lists `Telephony Tx`/`Telephony Rx` in the primary module
     * a call on the earpiece/speaker/wired headset still works. It is now a HW patch: `vendor.a6l.audio.call_out` = earpiece|speaker|headset, and there is no audio regression.
     * logcat `A6L_Q6VOICED` shows the ADM route writes (ENOENT without the kernel, then one retry per minute).
   * c. With the kernel ports:
     * `cat /proc/asound/pcm`: set `persist.vendor.a6l.btcall.dl_pcm` / `ul_pcm` (and `dl_ctl`/`ul_ctl` if the names differ)
     * `tinymix | grep -i "INCALL\|VOICE_PLAYBACK"` shows both routes at 1
   * d. Headset connected, place a call and select Bluetooth in the dialer:
     * `dumpsys media.audio_flinger` shows a software patch (Telephony Rx -> hfp output, BT SCO Headset Mic -> telephony tx)
     * `/proc/asound/card0/pcm<dl>c/sub0/status` = RUNNING
     * logcat `A6L_Q6VOICED btcall bridge ON` + `ctl 'VoiceMMode1 TX Mute' = 1`, and `A6L_AUDIO_ROUTE out=voice-bridge in=bridge-mic`
     * `HFP_SOFTWARE_ENCODING_DATAPATH` session started
   * e. Listen at both ends:
     * the remote party hears the headset mic, and **not** the phone mic (cover it, or scratch near it)
     * the headset plays the downlink, and nothing comes out of the earpiece or speaker
     * try CVSD and mSBC headsets
     * watch for drift/glitches over 5+ minutes (two clocks: DSP and BT)
   * f. Mute in the dialer: the remote hears silence (the uplink route goes to 0 in tinymix). Unmute: the headset mic comes back and the phone mic stays muted.
   * g. Switch to the earpiece during the call: `bridge off`, the phone mic is unmuted and the earpiece plays. Switch back to BT. Turn the headset off during the call: the audio falls back to the earpiece.
   * h. If the remote hears nothing (CVP mute also kills in-call music): `setprop persist.vendor.a6l.btcall.cvp_mute 0`, then retry e.
   * i. Rollback: `setprop persist.vendor.a6l.btcall.bridge 0`, reboot.
