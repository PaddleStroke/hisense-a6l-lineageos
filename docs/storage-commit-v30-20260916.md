# V30: separate register inspection from command commit

V29 survived baseline register reads and interrupt masking, with another USB heartbeat afterward. Its next callback combined a second snapshot and CMD0 write; losing the callback's log cannot distinguish those operations. V29 returned to stock Android and host cleanup completed.

V30 inserts one four-second delayed-work boundary after the pre-issue register validation. The next callback performs software request/power checks and exactly one command-register write, with no additional controller reads before that write. Each callback logs the host runtime-suspended flag, device runtime-PM status, usage count and whether the original request is still pending. Existing success/error/timeout branches and subsequent ordinary command/IRQ traces remain. No waits occur under the spinlock. No voltage, clock, DT, RAM-init, register ordering or block-write change is introduced.

The updated C register-model test passed 15 scenarios, including request/power changes specifically during the new pre-commit gap. The first kernel compile caught an invalid READ_ONCE of a bit-field in an optional debug print; that field was removed. The failed build was preserved, then the corrected kernel built successfully. Hardware behavior remains untested until the physical trial.

At the user's suggestion, targeted stock-binary analysis was added before this trial. Eight storage function ranges match the saved raw kernel. The ordinary 16-bit command-register store is present; Qualcomm source explains the observed register-logging scaffolding. No confirmed Hisense-specific fix has been identified. See [stock investigation](../research/stock-storage-binary-20260916/notes.md) for primary-source links, binary addresses and limitations.

Kernel, module load and full RAM QEMU checks passed. Candidate SHA256 `81c874f0a9112c09631bcf8f5250190ddb7ecc3f255317ac178f71ef3bbb03e8`, adapted kernel `f296f2f72194da3eb9fd9854b282366ba06d9148971f134067d81c5cfb1f442f`. RAM and module match V29 byte-for-byte. An early ABL checker invocation preceded packaging completion and failed only because no image existed yet; rerun after completed packaging is in progress. No phone write has occurred.

Captured ABL checks passed after packaging completed. Six writer protocol, four transition and eight collector checks passed. All nine tools and staged candidate match pinned hashes; remote offline inspection passed. V30 installation launched 19:49:08.364436 UTC, PID156802. Do not retry. Await full readbacks/desktop verification before manual Power. Capture/restore unused.

Installed 19:49:39.359024 UTC. All twelve copied readbacks passed independent desktop verification. Exact V29 predecessor and known BCB preserved; poweroff acknowledged, EDL absent, host services restored. User asked Power to normal Android; waiting. Installation USED; capture and restore UNUSED.

Stock Android baseline and host cleanup verified. Capture launched 20:04:55.084743 UTC PID157372 under GNOME A6L-v30-capture. Exact fastboot 18d1:d00d/1e529013 and active logger confirmed. User asked Recovery ~70 seconds, no filming. Capture USED; coordinator owns host-service pause pending Android return.

V30 physical capture complete: 200779 serial bytes, SHA256 `a5f4f097827808a2bb0179a69703f9ef17209574fc134b86b16c7f61eaa0f4de`. Pre-issue snapshot passed at kernel 35.296 s: status=0, present=03f800f0, signal=0, enable=00ff1003, clock=0007, power=0d. Runtime status 0 means active; usage count 1 and the original request still pending. Last heartbeat is kernel 39.008 s; USB disappeared at 20:06:24.416106 UTC. No step-2 callback or issue-return message was delivered. This narrows the failing region but cannot identify the exact instruction. The Qualcomm write wrapper can conditionally access CDR/DLL; “no additional reads” in the V30 description applies only to our outer state-machine block.

Normal Android return and host-service restoration verified at 2026-09-16T20:07:30.165975+00:00. Final session and analysis archived. V30 remains in recovery. Install/capture used; restore unused.
