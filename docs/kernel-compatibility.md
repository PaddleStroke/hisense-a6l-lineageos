# Kernel/runtime compatibility audit

Update: kernel-prototype-20260914.md records a built Linux 7.2.3 prototype with
BPF enabled and a successful diskless diagnostic boot in QEMU. This establishes
an experimental newer-kernel path; it does not establish A6L hardware or Android
vendor compatibility.

Measured stock: Linux 4.4.153-perf, CONFIG_BPF=y, CONFIG_BPF_SYSCALL unset,
CONFIG_BPF_JIT unset. CONFIG_BPF alone does not provide the eBPF syscall.
Binder, hwbinder, vndbinder, ashmem, seccomp and dm-crypt are configured.

Inspected downloaded Connectivity source at commit
7c04d866538e6f689823b769fb5c1e614ca0e295:
packages/modules/Connectivity/bpf/loader/NetBpfLoad.cpp.

Its loader rejects kernels below 4.9 unconditionally and has additional release
gates for 4.14, 4.19, 5.4, 5.10 and 5.15. Stock 4.4 fails the first gate before
BPF programs can be loaded. netbpfload.35rc specifies reboot_on_failure for the
loader. The newer source therefore provides a concrete reason an unchanged
system image cannot simply run on this stock kernel.

Removing version checks would not implement missing syscalls, map/program types,
helpers, verifier behavior or networking hooks. A port needs either a suitable
kernel with the Hisense hardware support carried forward, substantial verified
backports, or a deliberately maintained alternative networking implementation.
No gate has been removed, property spoofed, or kernel binary modified.

This is one subsystem audit, not a complete minimum-kernel assessment. Other
runtime interfaces, vendor drivers and ABI requirements still need checking.

See boot-compatibility.md for the independent kernel/boot work sequence and the
new boot-image provenance findings. This work does not require systemimage to
finish compiling.

The expanded investigation in kernel-investigation-20260914.md now resolves the
effective runtime gate to >=5.10 for this API 37.0/26Q2 product and records the
separate legacy data-encryption parser blocker, vendor module ABI evidence,
recovered e-ink callbacks, and boot-container roundtrip results.
