# V26: controller request and IRQ trace

V25 completed CMD0 chip-select and request preparation but lost USB after86 cmd0_dispatch_request, before return from __mmc_start_request.198629bytes SHA4c2eaa9387e8f2bdaaa0ab7818738a02d72a0524c5260fe1971c8fb0b864c296. No panic/block device. Android and host cleanup verified15:36:47.935613UTC.

V26 adds five paced CMD0 checkpoints before retune/request dispatch and before the SDHCI host lock. It adds a separate bounded64-event helper for command submission and IRQ processing. The helper prints passed software values, has no sleeps or MMIO, and uses the same A6L/device/boot-flag gating. Existing PRESENT_STATE read is assigned to a local then logged; no extra MMIO reads. IRQ status logs reuse existing intmask. No hardware settings change. Existing96x250ms pause cap retained. Source patches and trace-guard checks archived in firmware/extracted/storage-request-source-v26-20260916.

Adapted kernel410642b760aeb08b304014d74b83b60831a88cd924ee5a541d247ecf8266b494. Module0066e67c3d004c5b744629df87440005b50cdc9bc6cbb9e2296c95d0b07591cb differs fromV25 only in .note.gnu.build-id and .debug_line; executable/data sections identical. RAMff4f1d8e74dfe5e30ec13f0a2c3e54d948fb475b1ce310966315b565b8aac294 contains this matching module; init unchanged. Module QEMU passed; actualRAMtestrunning. NoV26phonewrites.

Both QEMU checks and source trace guards passed. Packaged candidatefdf8ee6e41c49b598b6b03ffec00ab564bc87af0913122b58b2ad5cb3987dcfc. First ABL invocation started before async packaging finished and found no image; no device action occurred. Packaging then completed successfully; ABL rerun is in progress.

ABL rerun reached all image checks but AVB emulation stopped before recovery was read (returnedfalse,noexception); original helper has30s walltimeout. Preserved incomplete report. New Test-CapturedAblAvbV26.py extends walltimeout to120s, retains30M instruction cap and exact assertions, adds PC/timeout status. Physical phone untouched; rerun required before staging.

ABL passed with120s walltimeout, same30M instructioncap and exactresult5/loadedimageassertions. Invalidvbmeta negativecontrolreturned6/no slot;no weakening of results.6protocol+4transitiontests and allstagedhashes/offlinepreflightpassed. Guardedinstallationlaunched;awaitverification.

Installed15:50:29.797169UTC. All12desktopreadbacksPASS;exactV25predecessor+knownBCBpreserved. Poweroff+hostcleanupcomplete. UseraskedPowernormalAndroid;awaitreply. Capture/restoreUNUSED.

Capture first attempt archived200000bytes. Global330s menu+capture deadline expired16:00:21.440639, only50.48s after15:59:30.961673 departure, before65s window. Last82cmd0_wait_ongoing_transfer;14non-sleepingevents show SDIOreset commandtimeouts0x18000 with IRQ returns. NO exactfault conclusion. PhoneUSB absent on subsequent check; useraskedPowerAndroid. CoordinatorwaitingAndroid240s.
Host-only repeat scripts prepared with freshcapture-probe-serial-user-v26-repeat: menu deadline applies only beforedeparture; full65s diagnostic deadline afterward; outer410s watchdog.6late-selection boundarychecksPASS. SameV26 image, noflashneeded. Upstreamnotesresearch/storage-request-upstream-20260916/notes.md;2018reportnotconfirmedmatchingfix.

Firstcoordinatorfinished16:04:22.584075UTC servicesrestoredTRUE,AndroidreturnFALSE;userPowerquestionpending. Finalsessionarchived. Repeat2hosttoolsstaged/hashverifiedbutnotlaunched. Actualmerged-captured-abl.dtbverifiedregc0c4000,c0c5000,c0c8000;interruptsSPI110/112;hc_irq/pwr_irq;sdm630-sdhci+v5compat. NoDTmappingchangeindicatedby2018thread.

Repeatcapturelaunched16:29:20.852505UTC PID148431 afterbaseline/port/hash/cleanhostverification. Exactfastboot+loggerconfirmed;useraskedRecovery60s,nofilming. SameinstalledV26,noimagewrite.

Repeatmenuwaitexpired16:34:55 beforephysicalRecoveryselection;coordinatorcleanup16:38:57 refuseddevelopmentUSB,hostpauseowned.16:53:27 exactdiagnosticUSBconfirmedandlatefreshreaderattachedPID149395,90sboundedcapture. Noadditionalreboot/write. NeedmanualhostresumeafterverifiedAndroid.

Latecapturefinished16:54:57.322997UTC;202686bytes SHA1733ada244fbc7eae5f91372cc5cab03446f0302417b8fa299af39c5528b41cf. Last91cmd0_sdhci_lock;14IRQevents show earlierSDIOresettimeout0x18000withhandlerreturn. Retune/requestdispatch/get_cdpassed. Nopostlockeventarrived,notproofoflockfailure(rapidfailurecanlosebufferedlogs). USBgone16:53:54.045219;moduleloadresult0,no panic/mmcblk. UseraskedPowerAndroid. HostpauseSTILLOWNED;mustmanuallyresumeafterAndroidbaseline. DTno-sdio/no-sdabsent;potentialcontrollednexttestskipunnecessarySDIOreset,notyetimplemented.
