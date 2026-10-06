# r6f modem startup and radio HAL cell-info fix — 2 October 2026

## r6g SIM retest later on 2 October

The cell-identity guard is installed in r6g. With the SIM inserted, boot
`be06d79c-c8b8-40e4-a488-3e403879c6d1` completed with radio disabled; live
kernel/logcat recording was started before enabling radio at uptime 115.90 s.
The modem reached running at 125.699477 s; rmtfs retained its read-only EFS
backing mode (`-r`). The radio HAL remained PID 1251 without the earlier
cell-identity abort. WLAN still reports `ready:mac-failed`.

Registration initially failed because **com.android.phone**, rather than the
radio HAL, repeatedly crashed with `ArrayIndexOutOfBoundsException: length=1;
index=1` in UiccController.java:1153. Physical slot resource lookup returned 1,
while the DSDS HAL correctly reports physical slots 0 and 1. Vendor API level
202604 means this Android version no longer enlarges the physical-slot array
to the logical phone count as it did for older vendors.

Android's supported read-only property `ro.telephony.sim_slots.count` was
unset. Supplying 2 for this boot at uptime 340.01 s stopped the crash loop;
Phone PID 15357 remained running in the subsequent observation. The same
property is added to `radio/radio.mk` for the next build. It is not yet in the
installed r6g images, and this test override does not survive reboot.

Pierre unlocked the SIM and confirms service. Logs report Orange F, PLMN
20801, home-network LTE registration and a connected data interface. Pierre
then completed a Google search over mobile data. This confirms SIM detection,
registration and usable Android mobile internet in this attended test.
Outgoing calls subsequently connected to Pierre's wife and voicemail, but
both directions were silent; speaker was also selected in the recorded calls.
Recorder later showed a speaking waveform but its playback was silent, while
some UI sounds remained audible. Voice audio is a separate blocker; see
`docs/audio-call-investigation-20261002.md`. SMS, VoLTE, suspend reliability and
long-term modem stability remain unverified. Slot 2 reports CARD_IO_ERROR
and needs separate investigation.

At uptime 661.048449 s, after the call tests, the kernel logged a GPU READ
TRANSLATION fault at IOVA 0x13b85000 (TTBR0 0x114ce6000, source 0x5030001).
Logging ended at 661.434 s, and the phone returned on boot
`1da5b42f-2f7b-4c0e-aa02-5874ad6f918c`; Pierre confirms an automatic reboot.
This is not evidence of a modem fatal error. The earlier radio/data results
stand, while whole-phone stability remains blocked by GPU faults.

Workspace archive:
`firmware/extracted/rom-r6g-20261002/r6g-modem-call-test-20261002.tar.gz`
(4,681,771 bytes). Recovery text after calls is also retained separately;
temporary raw diagnostic images were removed. The SIM is back in Pierre's
main phone, and the A6L is in recovery.

Laptop evidence: `rom-r6g/logs/live-modem-r6g-20261002T150325Z/`, including
`physical-slot-config.txt`, `physical-slot-count-test.txt`, crash excerpts and
`user-modem-results.json`.

## Earlier r6f investigation

Pierre confirmed a SIM is inserted. This attended test used the installed r6f
with ANGLE/SwiftShader, original screen geometry and e-ink off. Enabled
`persist.vendor.a6l.radio=1` after starting live kernel/logcat capture. EFS
write-through remained disabled: rmtfs used read-only backing partitions with
its RAM write shadow (`-r`). No partition image backup or flash was performed.

The installed rmtfs command uses `-P -o /dev/block/by-name`. rmtfs remained
running; the former `/dev/disk/by-partlabel/modemst1` open error did not recur.
Modem remoteproc reached running at uptime 1374.28 s. The last live kernel
record is at 1774.26 s, about 400 seconds later, without the former modem fatal
error in this recorded interval. This is a limited firmware startup result;
it does not prove network registration, calls or long-term modem stability.
The short-lived `a6l_radio` startup script finishing is expected; it is distinct
from the continuing `vendor.rmtfs` daemon.

Android's radio HAL immediately aborts and is repeatedly restarted:

```
Check failed: ratSpecificInfo.has_value() Cell identity not handled:
CellIdentity{noinit: false}
```

The stack reaches `structs::makeCellInfo` through
`RadioNetwork::getCellInfoListBase` during response-function connection.
The tracker can return a successful registration response with no cell identity
before a serving cell is known. The conversion helper requires a supported RAT
and aborts on `noinit` or unsupported CDMA. The fix returns an empty cell list
for those identities, preserving existing conversion for GSM, WCDMA, TD-SCDMA,
LTE and NR. It does not fabricate a cell or registered state.

Compiled the actual old and new method bodies with response/conversion stubs:
10 cases per version cover absent tracker, failed registration/signal responses,
unset identity, unsupported CDMA and five supported RATs. The old unsupported
conversion failure is reproduced; the new method avoids conversion and returns
empty. This checks method control flow, not QMI, Binder or physical registration.
The Android target `android.hardware.radio-service.a6l` compiled successfully
in 7 minutes 7 seconds. Source and binary are ready for the next ROM; the fix
is **not installed**. Evidence and the binary hash are in
`firmware/extracted/radio-cellinfo-20261002/`.

The phone entered s2idle at uptime 1774.21 s. Pierre reports it woke after
about 20 seconds, remained responsive but very laggy. USB did not enumerate
after waking or reconnecting the cable. The live stream ends during suspend,
so it does not establish the wake mechanism or cause of the delay. A normal
Android restart was requested but left a black screen without USB. Pierre
returned the phone to V75 recovery with USB unplugged before the forced reset.
The pending reboot observer was stopped; it never saw a new Android boot and
never disabled the persistent radio property. Radio therefore remains enabled
for the next Android boot and must be disabled in a guarded startup test.
Pierre removed the SIM for use in his main phone; reinsert it only for an
attended modem registration test.

Recovery metadata capture succeeded (`META_READ_OK`, 10 MiB diagnostic
partition, laptop-only filesystem check clean). `/sys/fs/pstore` is empty.
The persistent logger's current boot stops at uptime 280.37 s, before this
modem experiment at 1364.58 s. Its cached radio=0 property predates enabling
radio and does not show that the property was reset. It provides no trace of
the later wake or restart. The live laptop capture remains the relevant modem
evidence. No new ROM flash is ready for the GPU; sysmem failed physically.
Pierre requests prioritizing hardware graphics and bundling confirmed fixes
in one next build, with no separate camera/radio-only flash.

Additional finding: `vendor.a6l.wlan.state=ready:mac-failed`. Correct persistent
WLAN MAC assignment needs investigation before counting Wi-Fi as validated.
Stopping only `vendor.radio-a6l` did not keep the HAL stopped: framework requests
reactivated the lazy AIDL service. Network registration and calls were blocked
by this HAL crash loop; no Android telephony pass is claimed.

Laptop diagnostics:
`~/A6L-usb-20260915/rom-r6f/logs/modem-r6f-20261002T073539Z/`.
Compressed workspace copy:
`logs/r6f-install-20261002/modem-r6f-20261002T073539Z.tar.gz` (about 8 MB).
