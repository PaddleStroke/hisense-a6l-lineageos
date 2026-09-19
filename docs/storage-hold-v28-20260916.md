# V28: stop before issuing CMD0

V27 no-sdio did not fix the failure. Last delivered checkpoint90cmd0_sdhci_get_cd, versusV26checkpoint91cmd0_sdhci_lock; rapid subsequent failure can lose logs, so this is not proof of a lock or card-presence fault. V27 baseline+services restored18:30:51.597686UTC.

V28 tests whether command setup survives if CMD0 is never issued. After argument/transfer-mode setup and timer arm, but immediately before SDHCI_COMMAND write, the A6L+physicaldevice+storage_trace gated hook cancels the timer and returns from command-submission. The MMC request intentionally remains pending; no fabricated command completion, error recovery, further card commands, or persistent mounts are introduced. The RAM logger remains independent. This is strictly diagnostic and must not become production behavior.

No sleeps, lock dropping or additional MMIO in the new path. Existing64eventcap and96pacedcheckpointcap retained. V27no-sdio DT retained. Build/checks needed before any phone write.

KernelbuildPASS, adapted610940b3868a7f4b1d83e25950a76f9e420a850060d1798f7f67aae1bb59cce4; modulea57b0d7bca607b41b86c4f6c55dabfbf88990be2ab48726ce3e6bbde7eb7f984 differsV26onlydebug/buildidsections. RAMedb7c504033eeb96faac06cf6300c8f0e47a4267848641ee6d2eec0f7c4325d0;initunchanged. ModuleQEMU+actualRAMQEMU+packagingPASS. Candidate da70c8383db73370f75af2838f2701efcab5901a183d8318b7a67c760f3104b6. CapturedABLtestinprogress;no staging/phonewrite.

CapturedABLpassed.6protocol+4transitiontestsPASS;collectorbyteidenticalV27withfixeddeadlines. RemoteAndroid/port/cleanhost/freshpaths/all9hashes/candidate/offlineInspectPASS. INSTALL LAUNCH REJECTED byautomaticapprovalreview BEFORE execution: missingexplicitV28image/recoverywrite/poweroffapproval. No V28phonewrite. Useraskedexplicitapproval,awaitresponse;do notbypass. V27remainsinstalled.

UserexplicitlyapprovedV28install/readback/poweroff;reviewresolved. Recheckedbaseline/port/cleanhost/hash/freshpaths,launch18:40:11.018468UTC PID153379. Awaitfullreadbackverification;do notretryinstall.

Installed18:40:42.999889UTC;all12desktopreadbacksPASS. ExactV27predecessor+knownBCBpreserved,poweroffACK+EDLgone+hostcleanupcomplete. UseraskedPowernormalAndroid;awaitreply,NOFILM. Capture/restoreUNUSED.

Capturelaunched18:59:47.435708UTC PID153941;Androidbaseline/port/cleanhost/allhashesverified. Exactfastboot+loggerconfirmed;useraskedRecovery60s,NOFILM. CaptureUSED.

HOLD CONTROL PASSED19:01:07.476427UTC.202908bytes SHAe95b77f9662bf5f18f08d57d914995b4267c4d420f3469b3e8415f27d292d67a. Events1-13 showlock,LED,presence,ARG,transfermode,timerarm,cmd0_command_held,requestreturn;last92cmd0_wait_completion. ContinuedUSBALIVE46/48,no disconnect/panic/mmcblk. StorageNOTinitialized(intentionallypending). NarrowsfaulttoissuingCMD0orimmediatehardware/IRQresponse. UseraskedPowerAndroid;coordinatorawaiting240s;verifycleanup+archivefinalsession. NoV29yet.

Android return and services restored verified 19:02:54.514400UTC. Final session archived locally. V28 trial complete.
