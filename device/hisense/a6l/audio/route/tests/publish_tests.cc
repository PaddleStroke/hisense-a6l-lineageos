// SPDX-License-Identifier: Apache-2.0
// r5 review F62 (28 Sep 2026): the EXACT publisher + device-name helpers from the added lines of
// audio/patches/0002-a6l-call-route-mute.patch (extracted by run-tests.sh into publish_methods.inc) with recording
// Android port-config/patch/property fixtures, feeding the daemon's exact decide(). No Binder, no audio policy.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#define LOG(x) std::ostringstream()
enum class AudioDeviceType { OUT_SPEAKER, OUT_SPEAKER_EARPIECE, OUT_HEADSET, OUT_HEADPHONE, IN_MICROPHONE,
                             IN_MICROPHONE_BACK, IN_HEADSET, IN_TELEPHONY_RX, OUT_TELEPHONY_TX };
enum class AudioMode { NORMAL, RINGTONE, IN_CALL, IN_COMMUNICATION };
struct AudioDeviceDescription {
    inline static const std::string CONNECTION_BT_SCO = "bt-sco";
    AudioDeviceType type;
    std::string connection;
};
struct AudioPortExt {
    enum Tag { device, mix };
    Tag tag;
    struct Value { struct { AudioDeviceDescription type; } device; } value;
    Tag getTag() const { return tag; }
    template <Tag T> const Value& get() const { return value; }
};
struct Port { int32_t id; AudioPortExt ext; };
struct Patch { std::vector<int32_t> sourcePortConfigIds, sinkPortConfigIds; };
struct Config { std::vector<Port> portConfigs; std::vector<Patch> patches; };
static std::map<std::string, std::string> props;
static int sets;
namespace android::base { bool SetProperty(const std::string& k, const std::string& v) { props[k] = v; sets++; return true; } }
struct ModulePrimary {
    Config cfg;
    AudioMode mA6lMode = AudioMode::NORMAL;
    std::string mA6lCallOut, mA6lCallIn, mA6lMediaOut, mA6lMediaIn;
    Config& getConfig() { return cfg; }
    void a6lPublishCallRoute();
};
extern "C" {
#include "decide.inc"
}
#include "publish_methods.inc"
static Port dev(int id, AudioDeviceType t, std::string conn = "") { return {id, {AudioPortExt::device, {{{t, conn}}}}}; }
static Port mix(int id) { return {id, {AudioPortExt::mix, {}}}; }
static int fails, passes;
#define EXPECT(c) do { if (c) passes++; else { fprintf(stderr, "FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)
int main() {
    ModulePrimary h;
    using T = AudioDeviceType;
    h.cfg.portConfigs = {mix(1), dev(2, T::OUT_SPEAKER), dev(3, T::IN_MICROPHONE), mix(4), dev(5, T::OUT_HEADSET),
                         dev(6, T::IN_HEADSET), dev(7, T::OUT_SPEAKER_EARPIECE)};
    // reviewer case: output mix -> speaker, built-in mic -> input mix, headset plugged, NORMAL and IN_COMMUNICATION
    h.cfg.patches = {{{1}, {2}}, {{3}, {4}}};
    for (auto mode : {AudioMode::NORMAL, AudioMode::IN_COMMUNICATION}) {
        h.mA6lMode = mode;
        h.a6lPublishCallRoute();
        EXPECT(props["vendor.a6l.audio.media_out"] == "speaker" && props["vendor.a6l.audio.media_in"] == "main");
        EXPECT(props["vendor.a6l.audio.call_out"].empty() && props["vendor.a6l.audio.call_in"].empty());
        route_state s{};
        s.hp = 1; s.mic = 1; s.voice = 0; s.has_spk = 1; s.call_out = ""; s.call_in = "";
        std::string mo = props["vendor.a6l.audio.media_out"], mi = props["vendor.a6l.audio.media_in"];
        s.media_out = mo.c_str(); s.media_in = mi.c_str();
        route_choice c{};
        decide(&s, &c);
        EXPECT(!strcmp(c.out, "speaker") && !strcmp(c.in, "main-mic") && !c.unsupported);
    }
    int n = sets;   // unchanged state: no property churn
    h.a6lPublishCallRoute();
    EXPECT(sets == n);
    // VoIP on the earpiece + headset mic capture
    h.cfg.patches = {{{1}, {7}}, {{6}, {4}}};
    h.a6lPublishCallRoute();
    EXPECT(props["vendor.a6l.audio.media_out"] == "earpiece" && props["vendor.a6l.audio.media_in"] == "headset");
    // duplicated ringtone: both sinks listed
    h.mA6lMode = AudioMode::RINGTONE;
    h.cfg.patches = {{{1}, {2, 5}}};
    h.a6lPublishCallRoute();
    EXPECT(props["vendor.a6l.audio.media_out"] == "speaker,headset" && props["vendor.a6l.audio.media_in"].empty());
    // concurrent capture patches: the most recently created (last) wins
    h.cfg.patches = {{{1}, {5}}, {{3}, {4}}, {{6}, {4}}};
    h.a6lPublishCallRoute();
    EXPECT(props["vendor.a6l.audio.media_in"] == "headset");
    // modem call (legacy routing on the primary output): call_out published as before, media too
    h.mA6lMode = AudioMode::IN_CALL;
    h.cfg.patches = {{{1}, {2}}, {{3}, {4}}};
    h.a6lPublishCallRoute();
    EXPECT(props["vendor.a6l.audio.call_out"] == "speaker" && props["vendor.a6l.audio.media_out"] == "speaker");
    // no patches: cleared
    h.mA6lMode = AudioMode::NORMAL;
    h.cfg.patches.clear();
    h.a6lPublishCallRoute();
    EXPECT(props["vendor.a6l.audio.media_out"].empty() && props["vendor.a6l.audio.media_in"].empty() &&
           props["vendor.a6l.audio.call_out"].empty());
    printf("A6L_AUDIO_PUBLISH_TESTS %s pass=%d fail=%d\n", fails ? "FAIL" : "PASS", passes, fails);
    return fails != 0;
}
