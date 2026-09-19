# V14 and direct recovery replacement

The user authorized consecutive diagnostic replacements without restoring stock
recovery between each test. The normal Android boot image remains untouched.
The verified stock recovery payload and guarded restoration tool remain available.

V14 changes only the RAM diagnostic from V13, retaining the same Linux 7.2.3
kernel, device tree, regulator constraints and boot arguments:

- First minute: heartbeats, no USB gadget configuration.
- 58 seconds: visible notice; 60 seconds: create the ACM gadget without binding.
- 88 seconds: visible notice; 90 seconds: find UDC and bind the gadget.
- 118 seconds: visible notice; 120 seconds: open serial and send the journal.

The observer now requires a serial-write result and a heartbeat at or after
122 seconds, rather than accepting only earlier buffered heartbeats. QEMU can
exercise configuration without a controller; physical binding still requires
the A6L test. A blackout by itself does not prove a powered-off CPU or panic.

## Replacement guard

`RecoveryTransitionV14.py` validates complete saved reads before the single
recovery program operation. Allowed predecessors are exactly stock recovery or
the tested V13 image. Unknown/partial images are rejected. The complete devinfo
block must match the known unlocked spare baseline.

The old all-zero-only BCB precondition is deliberately replaced by an explicit
two-state allowlist: the full 4096-byte zero block, or the exact previously
captured `bootonce-bootloader` block. Both the complete hash and bytes are
checked. Other commands, nonzero tail bytes and truncated reads are rejected.
This known request selects the bootloader; it contains no factory-reset or
update instruction. Previous tests established return to original Android from
that state. It may still affect which startup menu is shown.

The BCB is never programmed or cleared by this workflow. Its bytes must remain
identical after installation. Identity, GPT, target-image hash, recovery-only
geometry, single-write rule and all full readbacks remain enforced. Subsequent
versions must explicitly pin their allowed predecessor instead of accepting
arbitrary recovery contents.

## Host access

All physical coordinators require an active GNOME user-session inhibitor for
idle and suspend. The wrapper uses the existing UID1000 session bus. Direct
systemd inhibition is denied for this SSH session, so it is not used. A separate
30-minute preparation inhibitor covers the build/staging period and is removed
when this run ends. Neither approach changes permanent power settings.

Routine operations use the already-installed scoped host helper and USB rules;
no password or general root shell is required. After a successful return to
normal Android, keep the diagnostic recovery installed for the next variant.
Restore stock when needed for recovery, stopping experiments, or user preference.

## Physical trial status

V14 installed with all twelve desktop readbacks verified. Predecessor was stock
recovery, with the exact captured bootonce-bootloader block preserved unchanged.
Recovery was selected at10:30:29UTC. The awake laptop saw no diagnostic USB device
or serial bytes before capture ended at10:33:56UTC. Normal Android returned at
10:35:06UTC, and host services and temporary inhibitors were cleaned up.

V14 remains installed in recovery for the next direct replacement. No stock
restore was performed. The final screen image is still needed to localize this
trial; lack of USB alone does not identify the failing stage.
