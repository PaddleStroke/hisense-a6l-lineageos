# V27: omit SDIO reset on soldered eMMC

V26 late capture reached91cmd0_sdhci_lock then USBdisconnected; kernel request atomic events did not reach host. Not proof that spinlock acquisition itself fails. Preceding SDIOreset had two command-timeout IRQs (0x18000) and handlersreturned. Proposed control: omit unsupported SDIOprobing on known soldered8bit eMMC. This is a hypothesis test, not a proven fix.

Only image change: boolean no-sdio on /soc@0/mmc@c0c4000. FullDTdictionary compared to V26 with exactlythatproperty added. Kernel410642b760aeb08b304014d74b83b60831a88cd924ee5a541d247ecf8266b494 and RAMff4f1d8e74dfe5e30ec13f0a2c3e54d948fb475b1ce310966315b565b8aac294 unchanged. Power/clocks/regulators/module/logging unchanged. Candidatea52505613120c8635d67947b76f937b6615b96665a408e75a119843183fcbdf6.

Packaging/roundtrip/exactDTdelta/capturedABL checksPASS. Sixprotocol+fourtransitiontestsPASS. Capturedeadline boundary checksPASS: menu1800s, full65s afterdeparture,outer1880s. Staged9toolhashes+candidate+offlineInspectPASS. V26Androidbaseline and hostservicesrestored17:30:04.783894UTC.

V27 guardedinstallationlaunched. Must inspectcompletion/copy12readbacks/Verify-StorageNoSdioReadbacks.py install beforeaskingPowerAndroid. InstallUSED;capture/restoreUNUSED. No recordingneeded.

Installed17:35:22.089929UTC. All12desktopreadbacksPASS;exactV26predecessor+knownBCBpreserved;poweroffACK+EDLgone+hostcleanupcomplete. UseraskedPowernormalAndroid;awaitreply. Capture/restoreUNUSED.

Capturelaunched18:19:37.143397UTC PID151808 underGNOMEA6L-v27-capture. Androidbaseline/port/cleanhoststate/toolhashesverified. Exactfastboot+loggerconfirmed;useraskedRecovery60s,no filming. CaptureUSED.

Capturefinished18:21:27.950801UTC;199258bytes SHAeeb73de7d97e89cefb1d247bbc7fce967b98b19c5ec59f393e634eb14aabfb91. Last90cmd0_sdhci_get_cd;moduleloadsuccess;zeroSTORAGE_EVENT,soearlierSDIOresettimeoutIRQsabsent. USBgone18:21:16.303762,no panic/mmcblk. SkippingSDIOresetdidnotresolvefailure. Lastdelivered90versusV26last91doesnotproveget_cd/lockfaultduebufferedlogging. UseraskedPowerAndroid;coordinatorwaiting240s,verifycleanupthenarchivefinalsession.
