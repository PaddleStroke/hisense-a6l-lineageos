# Returning physical USB work to Windows

The original switch to Linux was a fastboot investigation, not a finding
that Windows could not communicate with the A6L.

* Windows first enumerated fastboot without a driver (problem code28).
* USB forwarding through usbipd/WSL enabled queries, but repeated identical
  requests produced inconsistent OKAY/FAIL replies. The precise mechanism
  of that WSL failure remains unexplained.
* Direct Linux also initially failed. A tested Linux setup using the Google
  client, a bootloader-only NO_LPM quirk, and paused fwupd passed queries,
  reboot and later RAM transfers. This is why physical work stayed there.
* Earlier, native Windows EDL via Qualcomm's signed COM driver successfully
  produced the firmware/calibration backup with independent partition reads.

Sources: docs/kernel-prototype-20260914.md,
docs/bootloader-inspection-20260914.md,
docs/fastboot-transport-20260914.md,
docs/linux-laptop-20260915.md, and the successful-backup section of
docs/bringup.md. Historical locked/green state in those files is superseded
by the later authorized unlock; it is not the current phone baseline.

Current diagnostics install/restore recovery through EDL and use the
fastboot menu without sending a fastboot client command. Native Windows EDL
is therefore a viable route to investigate now. The Android builds already
run in WSL on the desktop and need no migration or rebuild for this change.

## Required transport work

The current guarded writer is Linux-specific: root/hostname checks, sysfs
port3-2 selection, SIGALRM, ownership changes, PyUSB endpoints and Linux
process supervision. DiagnosticRecoveryProtocolV11 writes directly through
EP_OUT and requires exact byte counts, including explicit empty USB writes.

The older Windows backup uses patched pySerial through COM. It preserves
the one-shot Sahara HELLO, uses finite timeouts and single-transfer Sahara
writes. However, its generic write helper retries exceptions and continues
after partial writes. Those behaviors must not be inherited by a persistent
recovery writer. Windows write(b'') is also not proof of an actual USB ZLP;
the Firehose serial path requires its own validation.

Before any Windows write:

1. Pin the local EDL library, programmer and payload files; retain the exact
   Sahara serial/HWID/PK hash checks before programmer upload.
2. Discover only the expected EDL interface; do not assume COM3 stays fixed.
   Establish the spare via ADB before its mode change and Sahara afterward.
3. Exercise a bounded read-only connection and verify both GPT regions,
   existing recovery, devinfo, BCB and vbmeta against the saved evidence.
4. Implement a serial write path with exact counts, finite timeouts, no
   retries after ambiguous/short writes, explicit initial/final ACK checks,
   and an independent external worker deadline suitable for Windows.
5. Preserve the fixed LUN0/512-byte sector recovery extent (start917504,
   count131072), one hash-pinned payload and one program attempt. Retain the
   raw-mode state guards and rejection of erase, patch and other partitions.
6. Read back all64MiB of recovery and the other fixed regions independently
   before permitting a reset or poweroff. A successful backup does not
   itself validate Windows writes.

V12 cleanup is complete: stock recovery restored and all twelve desktop
readbacks verified; stock recovery Reboot completed at 08:20:38 UTC.
The spare is now physically connected to Windows. No Windows persistent
write has been performed.

## V12 result relevant to the next host test

The user's photograph proves RAM userspace READY, debugfs read-only mount,
UDC a800000.usb, successful gadget.0 probe and USB gadget setup success.
The user reports the screen then went off. Without the host capture, this
does not prove actual poweroff or USB enumeration, nor establish a cause.
SSH was unreachable again during log retrieval. Screen evidence is saved
under captures/usb-only-screen-20260916.

## Native Windows read qualification passed

At 08:30:46–08:30:52 UTC the fixed read-only worker identified the exact
Sahara serial/HWID/key hash and verified six regions over COM3, including
the complete 64 MiB stock recovery and zero BCB. No program, erase or patch
was allowed. Reset was acknowledged only after all comparisons passed.
Stock Android returned and baseline properties matched at 08:31:15 UTC.
Five offline guard tests and actual serial-import checks passed beforehand.
The six saved files were independently hashed again afterward. Evidence:
`captures/windows-edl-readonly-v1`. This validates reads, not persistent writes.

## Prepared Windows V12 trial

`WindowsRecoveryTrial.py` now provides fixed install and restore operations.
The COM protocol uses five-second exact-count writes without retries or empty
COM writes, and configures ZLPAwareHost=0. All six regions are checked before
programming, with a single fixed recovery write and twelve saved readbacks;
post-write comparison precedes any power action. An external spawned worker
has a 120-second deadline. Eight offline tests passed, including partial
writes, timeouts, missing ACK, incorrect payload/geometry, operation gates,
restore checks and actual spawned-process termination.

`Collect-WindowsProbe.py` is prepared for receive-only capture and PnP snapshots.
Windows PnP inspection succeeds outside the sandbox. This observer has not
yet captured a physical diagnostic boot.

Automatic approval review rejected the actual install before process start,
requiring explicit user approval for this Windows firmware write. Approval
is pending; Android remains running and stock recovery remains installed.
No physical persistent COM transfer is yet validated.

## Physical programming FAILED — cleanup required

The user explicitly approved the Windows V12 install after the approval
review block. At 08:39:15 UTC the supervisor began; all six pre-reads matched.
The program request received ACK/rawmode=true. COM writes confirmed
9,437,184 bytes before SerialTimeoutException at 08:39:39 UTC. The next
transfer length is ambiguous; no retry, final ACK, post-read or power command
occurred. A subsequent receive-only session captured malformed-XML/NAK from
the programmer. Its exact cause is unresolved; Windows read success does not
validate programming. The writer preflight is now disabled.

User confirms normal Android restarted, then moved USB back to the laptop.
Recovery may be partial and must be restored before another Recovery boot.
New Linux rollback tools preserve the previously proven V12 transport and
checks, changing only one-shot capture names and pinning the new worker hash.
They are locally prepared but not yet staged because laptop SSH remains
unreachable even after a Wi-Fi reconnect. Requested current hostname -I.

## Stock rollback verified complete

Linux restored the full stock recovery at 08:51:17 UTC; Android and services
were verified at 08:51:41 UTC. All twelve copied files independently passed
desktop hashes. GPT regions, devinfo, vbmeta and zero BCB were unchanged.
No additional recovery-menu cycle is required.

The failed recovery image is exactly the first 1 MiB of V12 followed by the
remaining 63 MiB of stock recovery. Windows accepted 9 MiB before timeout,
but accepted-byte counts did not establish device progress. Premature
raw-mode exit/packetization cause remains unresolved. Native COM programming
is disabled; known Linux restore succeeded.

SSH became reliable for evidence transfer after a temporary Windows neighbor
entry pinned the laptop MAC, independently confirmed over SSH. The desktop
previously showed an unresolved neighbor while reverse ping succeeded. This
locates the network failure at address resolution but does not establish why
ARP replies were lost. The temporary entry is removed after this verification.
