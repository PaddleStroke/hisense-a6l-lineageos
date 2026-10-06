# Telephony flows: emergency, MMS, USSD (29 Sep 2026)

Worker: telephony-flows. Offline only: no phone, no adb/fastboot/EDL, nothing built with `m`. **No real emergency
number was dialled or will be dialled by any test here**: every emergency "dial" goes to the in-process fake modem.
Lands in the next build (r6). The APN list and carrier config belong to the Android-side carrier/APN work
(docs/android-overlays-carrier-updater-20260929.md). They were re-read and **not changed**: this worker only adds a
read-only MMS check.

## What changed (radio HAL, `device/hisense/a6l/radio/`)

| Area | File | Change |
|---|---|---|
| USSD codec + requests | `qmi/include/a6lqmi/ussd.h`, `qmi/src/ussd.cc` (new) | QMI VOICE Originate USSD No Wait 0x43 (+ its indication), sync Originate 0x3A (fallback), Answer 0x3B, Cancel 0x3C, USSD indication 0x3E, Release indication 0x3D. The layouts come from the public QMI VOICE IDL (libqmi `qmi-service-voice.json`) and match what ModemManager does (ASCII when the text is 7-bit, else big-endian UCS2; the UTF-16 TLV is preferred when decoding). The input is strictly UTF-8 checked, at most 182 bytes. |
| USSD indications | `qmi/src/services.cc` `voice::indicationRegister` | + TLV 0x16 = 1 (USSD notification events). The other TLVs are unchanged. |
| | `hal/ModemCore.{h,cpp}` | New `Listener::onUssd(Event)`. The 0x3E, 0x3D and 0x43 indications are parsed and posted to the worker thread. Malformed ones are dropped. The USSD text is never logged (only the length). |
| USSD dialogue | `hal/TelephonyFlows.h` (new, pure, host-tested) | `flows::UssdSession` chooses Originate or Answer and maps each result to an Android `UssdModeType`. Network asks for an answer (user action REQUIRED) -> REQUEST, and the next `sendUssd` is an Answer. Text without REQUIRED -> NOTIFY. Release -> NW_RELEASE. Originate result with an error -> NOT_SUPPORTED, with text -> NOTIFY, with nothing -> NW_RELEASE. Modem lost with an open dialogue -> NOT_SUPPORTED. |
| | `hal/RadioMessagingVoice.cpp`, `hal/RadioImpl.h` | `sendUssd` and `cancelPendingUssd` replace the generated "not supported" stubs. `sendUssd` runs on its own `a6l-ussd` executor, because the sync fallback can block for up to 100 s and must never hold up call control. `cancelPendingUssd` runs on the call executor; a NO_EFFECT answer counts as cancelled. An unbound slot 2 never uses SIM 1 (R3 rule). |
| Emergency numbers | `hal/TelephonyFlows.h` `emergencyNumbers()`, `RadioMessagingVoice.cpp` | The list reported as SOURCE_MODEM_CONFIG: 112 and 911 always. With no SIM, + 000/08/110/118/119/999 (3GPP TS 22.101). Camped on MCC 208, + 15 (ambulance), 17 (police), 18 (fire) and 196 (sea rescue), all with mcc 208, so they are never reported abroad. The list is re-sent when the SIM or the serving MCC changes it. The framework ECC database (TeleService eccdata) still adds FR 112/15/17/18/114/115/119. |
| "Emergency calls only" | `hal/RadioNetworkData.cpp` `buildRegState` | Voice registration only: when NAS says not registered but camped (a radio interface is listed: no SIM, PIN-locked SIM, rejected SIM) -> the `*_EM` RegState, so the status bar says "Emergency calls only" instead of "No service". Registered states are unchanged. |
| emergencyDial | `hal/RadioMessagingVoice.cpp` | Returns RADIO_NOT_AVAILABLE when the modem is not up at all, so Telephony retries after radio-on. Before, the request went to a dead client. There is deliberately **no** SIM, PIN or service gate: the modem places emergency calls in limited service. The r5 F23 semantics are unchanged (isTesting never sends an emergency call type). |
| Test-call guard | `qmi/src/services.cc` `isWellKnownEmergencyNumber` | + 114, 191 and 196 (FR deaf-access, air rescue, sea rescue). `emergencyDial(isTesting)` to any of these sends nothing. 15, 17, 18, 112, 115 and 119 were already listed. |
| ECBM | `RadioMessagingVoice.cpp` (comment only) | GSM/UMTS/LTE CS emergency has no modem emergency callback mode, so `enterEmergencyCallbackMode` is never indicated. `exitEmergencyCallbackMode` answers NONE (nothing to exit). |
| MMS | `tests/check-mms-apn.py` (new, read-only) | Checks that Orange World (208-00/01/02, APN `orange`) carries `mms` together with `default` (MMS rides the default PDN), MMSC `http://mms.orange.fr` with no proxy, and that every Orange mms-only MVNO entry (`orange.acte`, proxy 192.168.10.200:8080) has an MMSC and port. It warns on the unpatched IPV6 source. |

### MMS: what the data path must do

- **Orange France SIM**: Lineage `vendor/apn` has one "Orange World" APN `orange` with type `default,dun,supl,xcap,mms`
  and `mmsc=http://mms.orange.fr`, with no MMS proxy. Android's DataNetworkController satisfies the MMS network request
  with the existing default data network, so no second PDN is needed. The only change the carrier worker made is
  protocol IPV4V6 (patch `rom/android/patches/vendor/apn/0001-*`). The Orange carrier config (carrier id 32 asset, used
  unmodified) supplies the MMS limits and UA.
- **Orange MVNOs** (C le mobile, NRJ, Carrefour...): these use a separate `orange.acte` mms-only APN (proxy
  192.168.10.200:8080), so the HAL opens a **second data call**. The DataCallManager supports up to 4 calls, each with
  its own QMAP mux id and `rmnet_data<N>`. This is host-tested: default `orange` plus `orange.acte` get distinct
  cid/mux/netdev, PAP credentials go on the wire, and tearing down MMS keeps the default call. It is **not proven on
  the phone**: only one IPv4 call has ever run (data3/ipa4).

## Tests (all offline)

- `radio/tests/telephony_flows_tests.cc` (added to `tests/run-host-tests.sh`): **190 passed, 0 failed**, three runs, g++
  ASan+UBSan. The real `hal/ModemCore.cpp` runs over the fake modem:
  - USSD codec: ASCII, UCS2 and UTF-16 surrogates; rejects overlong or surrogate UTF-8, text outside the BMP, and more
    than 182 bytes; bad framing.
  - USSD dialogue: all mode transitions, cancel, modem lost, refused requests.
  - Indication register carries 0x16. All 3 indications reach `onUssd`; malformed ones are dropped. Requests are
    checked on the wire.
  - Emergency with **no SIM, PIN-locked SIM and no service**: the core is ready and the dial goes out as call type 9
    plus category.
  - Test calls to 112/15/17/18/114/115/119/191/196/911 are refused with 0 requests sent.
  - Number list per SIM/MCC, and the EM registration variants.
  - Second MMS PDN.
  - HAL source contract (executors, no SIM gate in emergencyDial, USSD text never logged, EM variant for voice only).
- Full `run-host-tests.sh` suite (all older binaries, now with the new VOICE register TLV and `ussd.cc` linked in):
  see the result line in the ledger entry.
- `check-mms-apn.py` on the tree's `vendor/apn/FR.xml` (raw and after `a6l_apn_fixup.py`): see the ledger entry.
- AIDL compile check (clang `-fsyntax-only` with the NDK AIDL headers, the same recipe as the r5 review) of
  RadioMessagingVoice/RadioNetworkData/ModemCore/service/RadioSimModemConfig: see the ledger entry.
- No sepolicy change and no kernel module, so there are no stage dry runs or sepolicy check to run.

## Needs an attended test (after the first r6 install, Orange SIM)

1. **USSD**: dial `*#100#` (own number) and Orange `*144#` (balance/menu). Expect a dialog with the text. In a menu,
   answer `1`, then Cancel. `logcat -b radio | grep -E "USSD ind|sendUssd|onUssd"` should show kind/action/len only.
   If `originate(no-wait)` answers INVALID_QMI_COMMAND, the sync fallback is used (log `originate(sync)`). Also try an
   accented reply if a menu offers one (UCS2).
2. **MMS**: send and receive an MMS (picture) with Google Messages/AOSP Messaging. Check `dumpsys telephony.registry` /
   `dumpsys connectivity` for an MMS request satisfied by the default network (Orange). With an MVNO SIM, if available:
   a second `rmnet_data1` appears during the MMS.
3. **Emergency, never a real call**:
   - `dumpsys phone | grep -A20 EmergencyNumber` lists 112/911 plus 15/17/18/196 (mcc 208), and the no-SIM list with
     the SIM removed.
   - With the SIM removed or PIN-locked, the status bar says "Emergency calls only" (not "No service").
   - The "Emergency call" button is offered on the lock screen.
   - **Do not dial** 112/15/17/18. A real emergency test call needs the operator's test procedure; that is Pierre's
     decision, not part of the test plan.
4. ECBM: nothing to test on GSM/LTE CS.

## Needs Pierre

- Whether a supervised real emergency call test is ever wanted (operator test number or a pre-arranged call). By
  default: never.
- If USSD shows garbled text on Orange (DCS handling on this MPSS), a radio log of that session (text stays out of
  the log; the dcs/len are enough).
