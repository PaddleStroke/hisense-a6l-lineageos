# V37: bounded direct read-only firmware verification

V36 7.2.3 storage enumeration and USB passed; Android and host cleanup verified. The next test keeps the exact working kernel/module/DT/overlays/command line and changes only RAM userspace.

The forked reader checks uname7.2.3/aarch64, eMMC name hDEaP3/manfid0x000090, sysfs179:0 and controller c0c4000, block major/minor and exact125074145280-byte capacity/512-byte sectors. It opens only /dev/mmcblk1 using O_RDONLY|O_DIRECT|O_CLOEXEC|O_NOFOLLOW|O_EXCL. Only BLKGETSIZE64 and BLKSSZGET getter ioctls are used. No buffered fallback, device writes, persistent mounts, shell, RPMB or calibration/userdata accesses. Each exact-length pread is bounded to one of five pinned immutable ranges: primaryGPT1MiB, stockboot64MiB, dtbo8MiB, vbmeta64KiB, secondaryGPT22528bytes. Each is read twice (128KiB and64KiB requests) and SHA256 compared to independently verified backup slices. A25second alarm bounds the reader process; PID1 keeps draining USB logs. Reader starts32seconds into the RAM diagnostic, after successful module load. Success requires all10hash matches, childexit0 and a further heartbeat, not merely module load.

Read semantics reference: https://man7.org/linux/man-pages/man2/open.2.html and https://man7.org/linux/man-pages/man2/pread.2.html . O_DIRECT alignment is enforced for buffer/offset/length, errors fail closed. Exact hardware behavior still requires the phone test.

Nine host fixture tests passed on the final reader source, including corruption, short read, wrong kernel/product/manufacturer/device/capacity and restored success. Sparse regular-file fixtures replace only host identity/geometry for those tests. strace verifies direct read-only open and no writes to the fixture descriptor. Initial run lacked strace; installed it in WSL and completed the trace, then reran with the final SHA256 self-test source. Earlier host evidence is preserved underhost-r1. The ARM image will run the SHA256 abc known-answer test before scheduling reads.

BoringSSL libcrypto has no Android static variant. The diagnostic now uses libcrypto_static with one scoped visibility entry for //device/hisense/a6l; this non-FIPS variant is appropriate for diagnostic checksums. Patch archived in the read-test evidence. Android build in progress, then final source update/self-test rebuild required. No V37 image installed yet; V36 remains the known-good recovery.

Static-crypto build completed. Final rebuild now includes the SHA256 abc self-test; host checks rerun on that exact final reader source passed. Source hash0c474104712d8855da4d2185c8fe94cc2ed95d8fb651190fcf64955b8d56de08, ranges hash6e020ac57818725e1b831fd85e39107bdc3188c73827fa8b7e1ad3fb23178839. Collector planned with75second capture bound, ten exact hash matches plus readerPASS/childexit0 and continuing heartbeat required.

Final build, RAM package and ARM QEMU passed, including the SHA256 known-answer self-test. Same kernel's negative framebuffer guards were already verified in V36; only the new RAM userspace was retested. Captured ABL, boot-image roundtrip/exact kernel+DT preservation, six writer-protocol tests, four transition tests and strict collector acceptance tests passed.

Candidate SHA256 `7de9a96229140b066aed2724ccf2cb14b1a09cc9e44a12bfb25ee4543761b8e9`.
RAM SHA256 `63e00e0dd1f24e32ea6abd23649aa6c5d2b31b7428ae1e92ec6cfaa44367c09d`.
Init SHA256 `b9cb35bd6f48eb6b6a71bfa3b139c5303b68df327e6329405b480b557df06326`.
Kernel/module/DT are exact V36. Both passes total153268224bytes.

Nine manifest-pinned tools and candidate staged and hash-verified on laptop. Offline Inspect and stock fallback guards passed; exact stock Android/port3-2 and clean host verified. Installation launched ONCE 2026-09-17T11:44:24.230652UTC PID210327 under GNOME inhibition. Awaiting finished session and all12independently copied readbacks before requesting restart.

Installation completed 2026-09-17T11:44:55.113322UTC. Exact V36 predecessor and preserved BCB verified. Device readback passed; poweroff acknowledged, EDL gone, host services restored. All twelve copied readbacks independently verified on Windows. User asked to start normal Android before physical V37 capture. Capture/restore remain unused.

Capture launched once at 2026-09-17T12:00:03.923149UTC, coordinator PID210872, collector PID210957. Coordinator verified exact stock Android and clean host before reboot. Fastboot identity 18d1:d00d / 1e529013 and live logger/report confirmed. User asked to select Recovery, leave connected about60seconds; no filming. Physical read result pending.

Physical V37 passed: all ten expected hashes match, 153268224 direct-read bytes completed in4351ms; childstatus0 and configured USB heartbeats continued through66seconds. Capture finished12:02:15.609448UTC,168673bytes, SHA7d60b1e5097f7ca409ce4164b15aee0e404406941fd141c585f6e580f713b2b8. Original collector incorrectly waited untildeadline/returned1 because CHILD_EXIT used CRLF but predicate expectedLF. Rawreport and pinnedtools preserved. Generator now normalizesCRLF; Analyze-StorageReadV37.py validates actualcapture, bothlineendings and15negative/regression cases (17checks total). No repeat hardware test needed. User asked to returnAndroid; cleanup pending. Next milestone is Android integration, not another storage enumeration test.

Normal Android and laptop service restoration verified at12:05:05.127945UTC; finalsession archived. V37 complete.
