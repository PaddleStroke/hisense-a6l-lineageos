# bt-audio (29 Sep 2026, offline; docs/android-bt-audio-20260929.md): Bluetooth audio for the rom-v2 product.
# Inherited from rom/rom.mk. WCN3990 (btqca, HCI over UART) is driven by the AOSP stack through
# android.hardware.bluetooth-service.default (HCI_CHANNEL_USER). All audio data paths are SOFTWARE:
#  - A2DP source: AIDL example audio HAL module "bluetooth" (audio_policy_configuration.xml) creates the
#    IBluetoothAudioProviderFactory (android.hardware.bluetooth.audio-impl, inside com.android.hardware.audio) and the
#    stack encodes SBC/AAC/LDAC on the CPU -> ACL over HCI. No split-A2DP (the downstream btfm slimbus link does not
#    exist on the mainline q6 stack).
#  - HFP/SCO: audio-managed SCO (android.media.audio sco_managed_by_audio is ENABLED in cp2a) + HFP software data
#    path: PCM between the audio HAL and the stack through the HFP_SOFTWARE_* BluetoothAudio sessions, SCO packets over
#    HCI (eSCO data path 0 = HCI). CVSD transcoding by the controller, mSBC/LC3 transparent.
#  - LE audio (BAP/CSIP/VCP/HAP/...) off: no LE audio ports, no ISO support validated on WCN3990.
# Without the bluetooth.profile.* properties the AOSP Bluetooth app starts NO profile (A2dpService/HeadsetService
# isEnabled() = sysprop orElse(false)); the GSI system image sets none.
PRODUCT_VENDOR_PROPERTIES += \
    bluetooth.device.class_of_device=90,2,12 \
    bluetooth.profile.a2dp.source.enabled=true \
    bluetooth.profile.avrcp.target.enabled=true \
    bluetooth.profile.hfp.ag.enabled=true \
    bluetooth.profile.gatt.enabled=true \
    bluetooth.profile.hid.host.enabled=true \
    bluetooth.profile.hid.device.enabled=true \
    bluetooth.profile.map.server.enabled=true \
    bluetooth.profile.opp.enabled=true \
    bluetooth.profile.pan.nap.enabled=true \
    bluetooth.profile.pan.panu.enabled=true \
    bluetooth.profile.pbap.server.enabled=true \
    bluetooth.profile.asha.central.enabled=false \
    bluetooth.profile.bap.broadcast.assist.enabled=false \
    bluetooth.profile.bap.broadcast.source.enabled=false \
    bluetooth.profile.bap.unicast.client.enabled=false \
    bluetooth.profile.bas.client.enabled=false \
    bluetooth.profile.bass.client.enabled=false \
    bluetooth.profile.ccp.server.enabled=false \
    bluetooth.profile.csip.set_coordinator.enabled=false \
    bluetooth.profile.hap.client.enabled=false \
    bluetooth.profile.mcp.server.enabled=false \
    bluetooth.profile.vcp.controller.enabled=false \
    ro.bluetooth.a2dp_offload.supported=false \
    persist.bluetooth.a2dp_offload.disabled=true \
    ro.bluetooth.leaudio_offload.supported=false \
    persist.bluetooth.leaudio_offload.disabled=true \
    bluetooth.sco.managed_by_audio=true \
    bluetooth.hfp.software_datapath.enabled=true

# btcall (29 Sep 2026, docs/android-bt-audio-20260929.md): cellular call audio to the BT headset through an AudioFlinger
# software bridge. OFF by default (persist.vendor.a6l.btcall.bridge unset): the bridge policy is installed but only
# bind-mounted over the product policy by init.a6l.btcall.rc when the property is 1 at boot. Needs the kernel in-call
# record / in-call music ports (not built yet, see the doc); pcm numbers persist.vendor.a6l.btcall.dl_pcm / ul_pcm.
PRODUCT_COPY_FILES += \
    device/hisense/a6l/audio/bluetooth/btcall/audio_policy_configuration_btcall.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration_btcall.xml \
    device/hisense/a6l/audio/bluetooth/btcall/init.a6l.btcall.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.btcall.rc
