// Exact routing publisher from the integration patch + daemon decision function.
// Android configuration/property recording fixtures; no Binder or audio hardware.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#define LOG(x) std::ostringstream()
enum class AudioDeviceType { OUT_SPEAKER, OUT_SPEAKER_EARPIECE, OUT_HEADSET, OUT_HEADPHONE,
    IN_MICROPHONE, IN_MICROPHONE_BACK, IN_HEADSET, IN_TELEPHONY_RX, OUT_TELEPHONY_TX };
enum class AudioMode {NORMAL, IN_COMMUNICATION, IN_CALL};
struct AudioDeviceDescription {
    inline static const std::string CONNECTION_BT_SCO="bt-sco";
    AudioDeviceType type; std::string connection;
};
struct AudioPortExt {
    enum Tag {device,mix}; Tag tag;
    struct Value {struct {AudioDeviceDescription type;} device;} value;
    Tag getTag() const {return tag;}
    template<Tag T> const Value& get() const {return value;}
};
struct Port {int32_t id; AudioPortExt ext;};
struct Patch {std::vector<int32_t> sourcePortConfigIds,sinkPortConfigIds;};
struct Config {std::vector<Port> portConfigs;std::vector<Patch> patches;};
static std::map<std::string,std::string> props;
namespace android::base {bool SetProperty(const std::string& k,const std::string& v){props[k]=v;return true;}}
struct ModulePrimary {
    Config cfg;AudioMode mA6lMode=AudioMode::NORMAL;std::string mA6lCallOut,mA6lCallIn;
    Config& getConfig(){return cfg;}
    void a6lPublishCallRoute();
};
#include "audio_methods.inc"
static Port devicePort(int id,AudioDeviceType type){return {id,{AudioPortExt::device,{{{type,""}}}}};}
static Port mixPort(int id){return {id,{AudioPortExt::mix,{}}};}
int main(){
    ModulePrimary h;
    // Framework configuration explicitly routes the output mix to speaker and built-in mic to input mix.
    h.cfg.portConfigs={mixPort(1),devicePort(2,AudioDeviceType::OUT_SPEAKER),
                      devicePort(3,AudioDeviceType::IN_MICROPHONE),mixPort(4)};
    h.cfg.patches={{{1},{2}},{{3},{4}}};
    for(auto mode:{AudioMode::NORMAL,AudioMode::IN_COMMUNICATION}){
        h.mA6lMode=mode; h.a6lPublishCallRoute();
        assert(props["vendor.a6l.audio.call_out"].empty() && props["vendor.a6l.audio.call_in"].empty());
        route_state s{1,1,0,1,"speaker","main"}; route_choice c{};
        // Even supplying desired names cannot alter the daemon's non-modem-call branch.
        decide(&s,&c);
        assert(!strcmp(c.out,"headphones") && !strcmp(c.in,"headset-mic"));
        printf("F62 mode=%s requested=speaker/main published_route=empty daemon=headphones/headset-mic\n",
            mode==AudioMode::NORMAL?"NORMAL":"IN_COMMUNICATION");
    }
    h.mA6lMode=AudioMode::IN_CALL;h.a6lPublishCallRoute();
    assert(props["vendor.a6l.audio.call_out"]=="speaker");
    route_state call{1,1,1,1,"speaker","main"};route_choice c{};decide(&call,&c);
    assert(!strcmp(c.out,"voice-speaker") && !strcmp(c.in,"main-mic"));
    puts("F62 modem_call_positive_control published=speaker daemon=voice-speaker/main-mic");
}
