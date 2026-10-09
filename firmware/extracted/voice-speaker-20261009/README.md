# voice-speaker-20261009: speakerphone silences the call, in-call volume steps fail

Offline work only. No phone, adb, laptop or SSH was used. Nothing under `device/`, `logs/` or the WSL ROM/kernel trees
was changed; the module builds ran in overlays inside a private mount namespace (bms-20261009 recipe).

Sources: `firmware/extracted/radio-sim-20261009/sim-1009-1147/all.txt` (11:49:19-11:50:28) and `kmsg.txt`
(kmsg seq 5872-5960).

## 1. Speaker pressed = silent call (root cause)

**What the log shows.** At 11:49:34.42 the daemon logs `rx route earpiece -> speaker (reopen)` and closes the working
pair. It then sets `TERT_MI2S_RX Voice Mixer VoiceMMode1`=1. Capture `pcmC0D2c` opens, but playback fails with
`HW_PARAMS: Invalid argument` and the daemon logs `OPEN_FAIL`. It retries 11 times (5 fast attempts, then 0.5/1/2/4/4 s).
There is no kernel error line for the failure. Each attempt only prints
`A6L_Q6VOICE path 6 ... rx_port 0x1004` plus the failed volume restore. Whenever Pierre switches back to the earpiece,
the open works at once (`call audio recovered after N failed opens`).

**Mechanism:**
1. `q6voice-dai` starts the voice path from the front-end DAI **startup**, at PCM open
   (`q6voice_dai_startup` -> `q6voice_start`; both directions open = `q6voice_path_start`). That is why the kernel
   prints the TERT path (0x1004) before the playback HW_PARAMS fails.
2. When the playback front end opens, DPCM opens the routed back end TERTIARY_MI2S_RX.
   `dpcm_be_dai_startup()` sets `be_substream->runtime = fe_substream->runtime` (a6l-rom-r5-src soc-pcm.c:1840),
   so the back end works on the front end's runtime.
3. The TFA98xx codec DAI startup (`tfa98xx_startup()`, oot/tfa `src/tfa98xx.c:2418`) adds two constraints to
   `substream->runtime`:
   - `snd_pcm_hw_constraint_mask64(FORMAT, S16)`;
   - `snd_pcm_hw_constraint_list(RATE, {rates of the current profile})`. The A6L container (`tfa98xx.cnt` 0ecfff4c)
     has profiles music, ringtone, voice, voip, bypass and calibrate. All of them run at 48 kHz (every message file is
     `*.48000.*`), so the list is {48000}.

   Because of step 2, these constraints land on the **VoiceMMode1 front end**.
4. The q6voice front end only offers 8000 Hz mono S16 (`q6voice_dais[]`), and the daemon asks for 8000/1.
   The core refine therefore fails with -EINVAL inside `SNDRV_PCM_IOCTL_HW_PARAMS`, before any driver `hw_params`
   runs. That is why nothing is printed.
   - The machine driver's `sm8250_be_hw_params_fixup()` (48 kHz, 2 ch, S16 for every back end, deployed sm8250.c d84c4bc7)
     would have given the TFA a valid 48 kHz back end. It never gets that far.
5. The earpiece route goes LPI_MI2S_RX_0 -> msm8916 WCD. That codec adds no rate constraint, so the same 8 kHz front
   end is accepted.
   - Side effect: the speaker would also have worked by luck if MultiMedia1 had already been playing on TERT_MI2S_RX.
     The back end is then already open, and the TFA startup does not run again.
6. **"Acts like mute" both ways:** the CVP/MVM session only exists while both VoiceMMode1 PCMs are open. After the
   failed reopen, the earpiece pair is closed and nothing replaces it, so uplink and downlink are both gone until
   Pierre switches back.

**Stock does the same routing**, so the routing itself is not the problem. Stock `vendor/etc/mixer_paths.xml`
`voicemmode1-call speaker` = `TERT_MI2S_RX_Voice Mixer VoiceMMode1` + `VoiceMMode1_Tx Mixer INT3_MI2S_TX_MMode1`, and
`voice-speaker` = `TFA Profile voice`. `audio_platform_info.xml` has `SND_DEVICE_OUT_VOICE_SPEAKER` on TERT_MI2S_RX.
Our `mixer_paths_a6l.xml` and a6l-q6voiced already match all of this.

**Fix (module only, no boot image):** `kernel/0002-tfa98xx-no-rate-constraint-on-dpcm-back-end.patch`.
- In `tfa98xx_startup()`, return before building the rate list when the link is a DPCM back end
  (`snd_soc_substream_to_rtd(substream)->dai_link->no_pcm`).
- The back-end rate still comes from the fixup, and `tfa98xx_hw_params()` still rejects a rate that the selected
  profile does not support.
- Media (MultiMedia1 -> TERT) is unchanged: the S16 format mask is kept, and the back end stays at 48 kHz.

**Zero-build equivalent:** load the ROM module with `pcm_no_constraint=1`. The parameter is 0444, so it can only be
set at load time. In the ROM that means `snd-soc-tfa98xx.ko pcm_no_constraint=1` in `rom/modules/audio.txt`, which is
a vendor change. This variant also drops the S16 mask on media front ends. The live test uses it first because it
needs no new binary.

## 2. Volume steps fail (second bug)

`0x112c2` is `VSS_IVOLUME_CMD_SET_STEP`. The kernel struct matches downstream `vss_ivolume_cmd_set_step_t`
(u16 direction = RX 1, u32 value, u16 ramp).

Every attempt fails with APR status **1 = ADSP_EFAILED**:
- step 4 at `restore` (vocproc enabled and attached, before MVM start);
- step 5 `live` during the running call.

The status is not 3 (ADSP_EUNSUPPORTED). So the CVD knows the command, and the failure is not about the step value
or the ordering.

The difference from stock is calibration:
- The stock msm-4.4 voice driver registers the ACDB vocproc volume table
  (`VSS_IVOCPROC_CMD_REGISTER_VOL_CALIBRATION_DATA` / dynamic variant) before SET_STEP. The step indexes that table.
- The mainline q6voice creates the vocproc with `VSS_ICOMMON_CAL_NETWORK_ID_NONE` and registers no calibration at
  all, so the vocproc has nothing to index.
- `MUTE_V2` (no table needed) works on the same handle. The `tx mute restore` lines show no error.

**Answer to the question:** most likely neither `VSS_IVOLUME_CMD_SET_NUMBER_OF_STEPS` (0x112C1) nor the legacy
`VSS_IVOCPROC_CMD_SET_RX_VOLUME_INDEX` (0x110EE) fixes this, because both still need the volume table.
- This is an inference, not verified. `kernel/0003-q6voice-rx-volume-fallbacks.patch` settles it on the phone in one
  call (section 4, step D).
- Default `rx_vol_mode=-1`: SET_STEP first; on failure, NUMBER_OF_STEPS(6) + SET_STEP; on failure, RX_VOLUME_INDEX.
  Every result is logged.
- `rx_vol_mode` is writable at run time: 0, 1 or 2 forces one variant.

**Consequences and options:**
- The failure is harmless apart from noise: the HAL logs `setVoiceVolume ... Failed` and the call runs at the DSP
  default level.
- A daemon-side workaround through the codec digital gain is **not** possible. a6l-audio-route re-verifies the full
  mixer state every ~3 s and would undo it, and the TFA speaker has no codec gain in the voice path anyway.
- If step D shows all three variants failing, the real fix is to register a minimal volume calibration table. That
  needs memory-mapped calibration support in q6mvm/q6cvp; it is a separate task.

## 3. Daemon: no more silent call, bounded retries

**Patch:** `daemon/0001-a6l-q6voiced-rx-route-fallback.patch` (source plus host tests; `git apply --check` OK on the
repo).
- After `FALLBACK_AFTER`=2 failed opens of a newly requested RX route (~250 ms), the daemon reopens the last route that
  worked in this call. If nothing worked yet (a call that starts on speaker), it uses the earpiece port.
- The refused route is retried 3 more times, at 5 s, 20 s and 60 s. Each retry costs a gap of about 0.1 s on the
  working route. After that it is "given up for this call".
- Asking for another route clears the state. Pressing speaker again retries at once.
- With no working route at all (card loss), the old F19 capped backoff still applies.
- `status` appends ` want=speaker fallback=1` while the call runs on the fallback.
- On a playback EINVAL, a one-line hint names the TFA constraint.

**Build and test results:**
- Host tests: `daemon/tests/q6voiced_tests.c` adds `test_rx_fallback`. With ASan/UBSan the result is
  **PASS 161/0** (`daemon/host-test.log`).
- Android binary: `daemon/a6l-q6voiced`, NDK r27c, API 34, `-DA6L_Q6VOICED_LOGCAT -llog`, stripped. NEEDED is liblog,
  libc and libdl. For the ROM, Soong builds it from the patched source.
- On Pierre's unit the earpiece is water-damaged, so the fallback gives uplink only. Still better than a dead call.

## Modules (keyless, verified)

| file | sha256 | srcversion (ROM) |
|---|---|---|
| kernel/modules/snd-soc-tfa98xx.ko (0002) | 1f68eb8d776a7f97c047d523aa7efa432ea2ed59e96dff26cc3617d47fbc9175 | 6160AC7A3F7C11A6A6F5A6B (0E6E59E314786C7B15E17D7) |
| kernel/modules/q6cvp.ko (0003, optional) | 595c58fd402f5d3ed3e4b0b80bbd58845024b300bff146b5b2a7d646137cf4e1 | 147F6B5C5272F6717BD9DCE (37BAECDC833B5006FED62B9) |
| kernel/modules/q6voice.ko (0003, optional) | 86cc29400d9bfe7bd9a93b3896b0e9e725eb7e00d9fd16fbfd84b43b9a6e7a93 | 724F39A52B640E83FD8952F (0E6A48426EFB4788EFBBEAF) |
| daemon/a6l-q6voiced | 5ac5a184b8bb9e1ffc7daf565164fc8c1f62b4565532a4178cbb9bc5b74930cb | - |

**`kernel/build.sh`** (output in `kernel/build.out`):
- First it rebuilds the **unmodified** deployed sources. All four rebuilds are **byte-identical** to the ROM prebuilts
  in `/home/a6l/android/a6l-lineage24/device/hisense/a6l/rom/prebuilt/vendor/lib/modules`:
  - TFA b855076b, from `out-a6l-rom-r5/oot/tfa` = `/home/a6l/audio3/tfa`;
  - q6cvp 62149164, q6voice 4527a4ec and q6voice-dai adad28e2, from `scratch/voice-controls-20261003-063728/candidate`
    (r5 qdsp6 + tx-mute + rx-volume).
- Then it applies the patches.
- Toolchain: clang r584948, `LLVM=1`, `LOCALVERSION=+`, `llvm-strip --strip-debug`.
- The protected out-dir files are unchanged.

**`kernel/abi-check.txt`:**
- vermagic `7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64` and depends are identical to the ROM.
- Import CRCs: TFA 105/105, q6cvp 10/10 and q6voice 33/33 match r5 `Module.symvers` (= the running kernel's, see
  bms-20261009) plus the deployed q6afe/q6mvm/q6voice-common exports. The TFA has no new imports.
- q6voice export CRCs are unchanged. The ROM `q6voice-dai.ko` (all 11 q6voice imports OK) is kept, and its rebuild is
  identical, so it is not shipped.
- q6cvp only adds two exports.

## 4. Live test plan (Pierre, SIM call)

Preconditions:
- fresh boot, root adb, no call in progress, battery > 30 %;
- a second phone to call;
- do not set stay-awake (the audio back-end wakeup source holds the device awake during the call).

Logs go outside `logs/`, for example into `firmware/extracted/voice-speaker-20261009/live-<time>/`.

```sh
adb root
adb shell mkdir -p /data/local/tmp/vs
adb push phone/vs-swap.sh daemon/a6l-q6voiced kernel/modules/snd-soc-tfa98xx.ko kernel/modules/q6cvp.ko kernel/modules/q6voice.ko /data/local/tmp/vs/
adb shell sh /data/local/tmp/vs/vs-swap.sh status
adb logcat -b all -v threadtime > live/logcat.txt &      # host side
adb shell cat /dev/kmsg > live/kmsg.txt &
```

**A. Daemon fallback (ROM kernel modules, so the speaker is still broken).**

Run `adb shell sh /data/local/tmp/vs/vs-swap.sh daemon` and expect `running the patched daemon`. Then call, wait for
ACTIVE, and press SPEAKER.

Expected in logcat (`grep a6l-q6voiced`):
```
playback HW_PARAMS: Invalid argument
hint: the RX back end refuses the 8000 Hz mono voice front end ...
rx route speaker refused (2 failed opens): falling back to earpiece, retry 1/3 in 5000 ms
call audio on fallback route earpiece (requested speaker)
```
- The far end must keep hearing you; the gap is under 0.5 s.
- About 5 s later: `retrying refused rx route speaker (1/3)` and then straight back to the fallback. The same follows
  at 20 s and 60 s, then `given up for this call`.
- Press speaker off and on again: one immediate retry.
- No retry storm in kmsg: at most 2 `path 6 ... 0x1004` lines per retry.
- Hang up.

**B. Speaker fix, zero-build (ROM TFA with `pcm_no_constraint=1`).** Run
`adb shell sh /data/local/tmp/vs/vs-swap.sh param`. The script:
- stops q6voiced, audioserver, the audio HAL and a6l-audio-route;
- aborts if anything still holds `/dev/snd`;
- unbinds the card, reloads the TFA, rebinds the card and restarts the services.

Then:
1. Expect `pcm_no_constraint=1` in the status output. Play a ringtone or media on the loudspeaker to confirm it still
   works.
2. Call. When ACTIVE, press SPEAKER with the phone away from your ear. Expect in logcat:
   `rx route earpiece -> speaker (reopen)`, `playback ... open+prepared`, `OPEN card 0 dev 2`, and no `refused`.
3. Expect in kmsg exactly one `A6L_Q6VOICE path 6 tx_port 0x1035 rx_port 0x1004` per press.
4. Check both directions:
   - the far end's voice comes out of the loudspeaker (the first real downlink check: the earpiece is damaged);
   - the far end hears you from 30-50 cm (main mic, uplink).
5. Toggle speaker off and on 3 times. Each switch takes about 0.1 s, and speaker always comes back.
6. Mute and unmute in the dialer while on speaker: the far end loses and regains your voice.
7. Volume keys: expect `volume ... -> ERR 5` (known, section 2). The level does not change.
8. Optional: also run steps 2-7 with A's patched daemon, and in a second call that starts with speaker already on.

**C. Speaker fix, ROM candidate (patched TFA module).** Reboot, then run A, then `vs-swap.sh tfa`. Repeat B.2-B.6.
The status output must show `snd_soc_tfa98xx srcversion 6160AC7A3F7C11A6A6F5A6B` and `pcm_no_constraint=0`.

**D. Optional volume experiment (answers section 2).** Reboot, run A, then `vs-swap.sh q6v`. This loads the patched
TFA plus q6cvp/q6voice; the ROM's `cvd_mode=0` is kept and the ROM q6voice-dai is reloaded. During a speaker call:
1. Press volume up and down a few times, watching kmsg:
   ```
   A6L_Q6VOICE rx volume SET_NUMBER_OF_STEPS(6): r1, then SET_STEP(n): r2
   A6L_Q6VOICE rx volume SET_RX_VOLUME_INDEX(n): r3
   qcom-q6cvp ... command 0x112c1 / 0x110ee failed with error E
   ```
   E = 1 means EFAILED (no table, as predicted); E = 3 means EUNSUPPORTED (command not in this CVD).
2. If `r2` or `r3` is 0, check that the loudness really changes. Then pin that variant with
   `echo 1 > /sys/module/q6voice/parameters/rx_vol_mode` (or 2) and press the volume keys again.
3. Report the r1/r2/r3 values.

**Undo:** reboot. Every module and the daemon come back from `/vendor`.

## ROM integration (after B/C pass)
- **Daemon:** apply `daemon/0001-a6l-q6voiced-rx-route-fallback.patch` to the repo and to the WSL ROM tree; Soong
  rebuilds `a6l-q6voiced`. No sepolicy change is needed. Host test command: see the header of `tests/q6voiced_tests.c`.
- **TFA:** put `kernel/modules/snd-soc-tfa98xx.ko` (1f68eb8d) into `rom/prebuilt/vendor/lib/modules/` in the ROM tree,
  as was done for pmi8998_fg. Keep `0002` with the TFA source. The fallback is the `pcm_no_constraint=1` line in
  `rom/modules/audio.txt`.
- **q6cvp/q6voice (0003):** ship only if D shows a working variant. Otherwise volume needs the calibration work above.
- **Docs:** `docs/port-status.md` (speakerphone, in-call volume) and `docs/kvoice-20260924.md` ("DPCM with a voice FE":
  codec startup constraints apply to the FE runtime).

## Files
- `daemon/`:
  - `0001` patch; patched `a6l_q6voiced.c` and `tests/q6voiced_tests.c`;
  - `host-test.log`;
  - `a6l-q6voiced` (arm64).
- `kernel/`:
  - patches `0002` (TFA) and `0003` (q6voice volume);
  - `build.sh` and `build.out`, `*-build.log`, `abi-check.txt`;
  - `*.symvers` (q6voice/q6cvp exports, baseline and new);
  - `modules/*.ko`.
- `phone/vs-swap.sh`: live swap and status (daemon | param | tfa | q6v | status).
- WSL work dir (overlay uppers, unstripped objects): `/home/a6l/voice-speaker-work-20261009`.
