// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL, r5 review fix F6 (28 Sep 2026): call (uplink) mute through a6l-q6voiced.
// a6l-q6voiced owns the ALSA control "VoiceMMode1 TX Mute" (kernel patch kernel/kvoice/a6l-q6voice-tx-mute-v75: CVP
// VSS_IVOLUME_CMD_MUTE_V2 on the ADSP voice path, applied live and restored at every call start) and serves one
// request line per connection on /dev/socket/a6l_q6voiced. The audio HAL (IModule.setMicMute, tree patch
// audio/patches/0002) uses the same socket, so there is one applied mute state. No Android dependencies (host tests).
#pragma once

#include <string>

namespace android::hardware::radio::a6l {

constexpr const char* kQ6voicedSocket = "/dev/socket/a6l_q6voiced";

struct VoiceMuteReply {
    enum Kind { Ok, Unsupported, DspError, BadRequest, NoDaemon, Protocol };
    Kind kind = NoDaemon;
    int err = 0;        // errno reported by the daemon (ERR <errno> ...) or by the socket calls
    bool mute = false;  // valid when kind == Ok
    std::string text;   // reply line (or local error) for the log
    bool ok() const { return kind == Ok; }
};

// Pure: parse one reply line of a6l-q6voiced ("OK mute=N" / "ERR <errno> <text>").
VoiceMuteReply parseQ6voicedReply(const std::string& line);
// Sends `cmd` ("mute 0|1", "getmute") and waits for the reply line (timeoutMs covers the DSP command).
VoiceMuteReply q6voicedRequest(const std::string& cmd, const char* path = kQ6voicedSocket, int timeoutMs = 4000);
inline VoiceMuteReply setVoiceTxMute(bool mute, const char* path = kQ6voicedSocket) {
    return q6voicedRequest(mute ? "mute 1" : "mute 0", path);
}
inline VoiceMuteReply getVoiceTxMute(const char* path = kQ6voicedSocket) { return q6voicedRequest("getmute", path); }

}  // namespace android::hardware::radio::a6l
