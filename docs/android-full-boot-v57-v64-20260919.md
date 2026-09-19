# V57–V64 — from HintManager to a complete Android boot in the VM (19 September 2026)

All work here is **offline** (diskless QEMU `virt`, TCG). The phone was not touched:
stock Android with the V46 diagnostic recovery remains the last verified state.
Nothing below is evidence that A6L hardware works; every daemon runs without A6L hardware behind it.

## Result

V64 r1: genuine SystemServer starts **every** system service, passes boot phases 480–1000,
unlocks user 0 and starts applications. Observed in the log: `SurfaceFlinger Boot is finished`,
`Starting phase 1000`, `Displayed com.android.settings/.FallbackHome`, SystemUI (`com.android.systemui`)
started, LatinIME bound, and LineageOS `SetupWizardActivity` launched as top activity.
22 of 23 harness checks pass (the `cleanup` check fails, see below).

The run ended with an upstream NPE in `WebViewZygote.getProcess` because the WebView child zygote
aborted: `Failed to mount tmpfs to /data/data: No such file or directory`. V65 adds init.rc's
`/data/data` directory and `/data/user/0` symlink.

## V65 r1 addendum

With `/data/data` present the WebView zygote starts, `sys.boot_completed=1` is set and
`Displayed org.lineageos.setupwizard/.WelcomeActivity` is logged: the VM reaches the LineageOS welcome
screen (software rendering, no screenshot captured). About a minute later SystemServer dies in
`NetworkStatsService` with `synchronizeKernelRCU failed: -24` (EMFILE): the harness starts zygote with the
shell's 1024-descriptor limit, whereas init.rc grants 32768. V66 sets `ulimit -n 32768`.

## Steps

| Test | Added | Stopped at |
|---|---|---|
| V56 r1 | merged LineageOS change 434585 adapted (no AIDL Power HAL) | property allowlist: `sys.sysctl.extra_free_kbytes` |
| V57 r1 | VM property service accepts `sys.*`; logs `ctl.*` requests | StorageManagerService NPE — no vold |
| V58 r1 | real `vold`, `idmap2d` | NetworkManagementService waits for netd |
| V59 r1 | **runtime kernel V59** (Android networking built in), `a6l_socket_exec`, real `netd` | watchdog: AudioService blocked, no audioserver |
| V60 | early vold (apexd no longer waits ~60 s), `audioserver` | audioserver exits: no audio HAL at all |
| V61 r8 | AOSP example AIDL audio HAL as vendor APEX `com.android.hardware.audio`, effects + policy XML | BiometricService: no gatekeeper |
| V62 r3 | `gatekeeperd` (software fallback), `keystore2`; VM budget 300 s → 1800 s | AdbService (`/data/misc/adb`), keystore2 panics without KeyMint |
| V63 r1 | AOSP software KeyMint (`keymint-service.nonsecure`) | user 0 storage prepare fails → framework requests wipe |
| V64 r1 | 138 `/data` directories generated from `init.rc`, `vold_prepare_subdirs` | WebView zygote: `/data/data` missing |

## Findings that matter for the phone image

- **Kernel networking configuration.** The Linux 7.2.3 diagnostic configuration had xtables/conntrack as
  modules and lacked `XT_MATCH_BPF`, `XT_MATCH_OWNER`, `XT_TARGET_IDLETIMER`, `NET_SCH_INGRESS`,
  `NET_CLS_BPF`, `INET_DIAG_DESTROY`, `XFRM_USER`, and needs `NETFILTER_XTABLES_LEGACY` /
  `IP(6)_NF_IPTABLES_LEGACY` on this kernel generation. `tools/build-framework-kernel-v59.sh` records the
  full list; only `XT_TARGET_CONNSECMARK` could not be built in. The phone kernel needs the same set
  before netd/Wi-Fi/tethering can work. Android's out-of-tree `xt_quota2` is absent on mainline;
  netd started without it.
- **Audio.** audioserver exits when no audio HAL is declared, and waits for *every* `IModule` instance
  declared in VINTF. The real A6L image needs a complete audio HAL declaration + policy XML from day one,
  even before the codec/ADSP work; the example HAL is a usable placeholder.
- **Keystore.** keystore2 panics without a TEE-level KeyMint. Until the A6L TEE/KeyMint path exists, the
  software KeyMint is the only way to boot; that has security implications (no hardware-backed keys)
  that must be decided explicitly for a daily-use image.
- **Power HAL.** Still absent; the adapted LineageOS patch keeps HintManager alive without it.
- Three optional BPF programs still lack kernel BTF / Android FUSE-BPF (unchanged from V55).

## Harness limitations (not framework defects)

- The supervisor emulates init: no `ctl.start` except a special case for the lazy `idmap2d`; other
  `ctl.*`/`ctl.interface_start` requests return 0x18 and are logged as `A6L_PRIVATE_CONTROL`.
- `cleanup` fails since V59: application processes and daemons keep binderfs busy at teardown.
  The VM is disposable, but the check should be restored by reaping all processes before unmounting.
- No `lmkd`, `statsd`, `media.metrics`, `mediaserver`, thermal HAL; these log warnings only so far.
- TCG is slow: one run takes ~9 minutes; SystemServer has 900 s, the VM 1800 s.

## Reproduce

Inside WSL: `bash tools/run-framework-series.sh <series> <attempt>`; archives land in
`firmware/extracted/android-framework-v<series>-<date>-r<attempt>` (not in git), working copies in
`/home/a6l/kernel/framework-v<series>-r<attempt>`. Builds: `tools/build-framework-kernel-v59.sh`,
`tools/build-framework-supervisor.sh`, `tools/build-framework-module.sh <label> <modules…>`,
`tools/build-framework-audio-v61b.sh`.
