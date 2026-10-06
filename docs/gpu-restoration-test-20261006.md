# A6L GPU restoration diagnostic — morning test

Build/kit readiness is recorded in `firmware/extracted/pm-logging-20261005/status.json`.
Read the latest section of `handoff-20261005-claude.md` first. The phone was left
asleep on user request; overnight stability is unknown. This diagnostic has not
been flashed at initial writing. Do not confuse it with the failed resume-order
module34d552… already installed.

## Purpose and scope

The GPU transition after video reproduces a CP opcode fault/ringbuffer timeout.
Changing scheduler resume order did not clear it. We need evidence from the
initialization packet and hardware pointers, rather than another unchanged test.

The candidate records:

- Software runtime state and return values around force suspend/resume, actual
  runtime callbacks, initialization and scheduler restart.
- The first four real submissions, including runtime-get and initialization
  results that the original driver ignores. Error handling remains unchanged.
- Six ordinary CP registers, the initial nine ring words and eight words around
  the hardware read pointer, before software reset, after ring programming,
  before/after initialization flush and idle, and on the existing fault IRQ.

MMIO samples run only in existing powered A5xx initialization/IRQ contexts.
Those paths' power contract is assumed; this is not independent proof that
physical clocks were restored. No new register writes, resets, delays, memory
allocations, worker threads, voltage changes or lock changes in GPU power/work
paths. A small diagnostic-only control lock serializes arming/consumption.

Default off. Root-only `/sys/module/msm/parameters/restore_trace` accepts0off or
1arm; read2means active. Only the next A6L/A512 system-suspend callback consumes
the arm. Arm expiry300seconds, active expiry120seconds. Hard96generic and
12reserved fault lines, four submits, two samples per ordinary point and four
fault samples. One consumed capture per module load/boot; later rearm returns
EALREADY, even after explicit disarm or expiry. Read0 may therefore mean spent.
Do not retry on the same boot. Already claimed records may finish after disarm.

## Installation and preparation

1. Preserve any overnight failure logs before another Android boot. If USB is
   missing, establish the physical screen state; USB loss alone is not reboot
   proof. If recovery is needed, unplug before forced reset, then Power+VolDown.
2. Verify trace build/root-review/image-audit/stage receipts. New kit is intended
   as `~/A6L-usb-20260915/rom-r7c-gpu-trace-20261005`; wait for its verified stage
   receipt before using it. Flash vendor only with updater hash-only policy.
   Boot/system/DTBO must skip and protected invariants remain unchanged.
3. Verify installed boot, complete vendor prefix hash, mounted module hash,
   default-off trace parameter/600mode, quietconsole1, PMnone/prefix-1, healthy
   persistent loggers. Use the prepared trace runtime helper, not the old34pin.
4. On a new boot, prepare the same WFI-only comparison with the old
   `rom-r7c-pm-prefix-20261005` harness/log kit (whitelist unchanged). Save original
   idle settings; disable40non-WFI states. Verify owned trace/inode and four fresh
   CRC-valid rings before releasing its native setup guard. Start host collector.
5. Save fresh oldkit `logs/pm-prefix-comparison-current.json` with actual new boot,
   diagnostic module SHA, `gpu_restore_trace_ready=true`, and preparation proof.
   Never reuse a prior boot's ready record.

## One controlled comparison

Ask user for10seconds main rear video→Stop/save→Home, LCDawake, USBconnected.
They should leave Power alone until the result is recorded.

`run-prefix-gpu-trace.py 42` adds reviewed trace controls to the already reviewed
selected-prefix controller. It checks new-kit payload/ready pins and trace0,
acquires the original harness setup guard, proves four fresh rings, arms trace
immediately before GO while guarded, then runs only the GPU-prefix comparison.
After return it requires actual callback identities to include5000000.gpu and
trace2. It preserves the original120second same-boot observer and host collection.
It explicitly disarms only on the same verified boot; otherwise it records
unavailable cleanup/newboot and relies on bounded expiry. No auto reboot/retry.

Prefix42 is a serial diagnostic unwind, not real system sleep. Counted callbacks
can change order between boots; interpret identities and trace markers. A
returned syscall and zero wrappers do not establish stability. Do not proceed
to ordinary video→sleep merely because callback proof passes.

## Preserve and interpret

- Save returned/private trace and receipt, host kmsg/logcat rings, observation,
  userdata/metadata current/previous rings, boot IDs and any GPU devcore record.
  Decode rings with CRC-health report. Host tail captured faults missed by disk.
- Match `A6L_GPU_RESTORE epoch=1` to this exact boot and controlled interval.
  Compare hardware versus software pointers before reset and CP_ME_INIT contents
  before/after flush. Compare ring base to recorded IOVA and hardware pointer
  neighborhood to packet words. Note WHERE_AM_I, current ring and preemption.
- If initialization returns an error before first normal dispatch, preserve that
  chain. If the packet is valid but hardware read pointer retains stale state,
  investigate reset/power-collapse semantics. If registers/packet look correct,
  investigate firmware/memory visibility and actual job/power sequencing.
  These are interpretation branches, not proven causes before the trace exists.
- Missing data or disappearance is not a pass or a terminal reset stack. A
  diagnostic can alter timing; absence of the crash alone does not clear the bug.

No e-ink/audio/encoder changes are bundled. Preserve the two October2 shipped
GPU fixes and current resume ordering when rebuilding. No full dev-ROM backups.
