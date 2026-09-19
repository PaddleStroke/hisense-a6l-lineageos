/* Real AIDL allocator + stable mapper + DRM composer service integration. */
#include <aidl/android/hardware/graphics/composer3/IComposer.h>
#include <aidl/android/hardware/graphics/composer3/BnComposerCallback.h>
#include <aidl/android/hardware/graphics/composer3/DisplayConfiguration.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <hardware/gralloc.h>
#include <poll.h>
#include <aidl/android/hardware/common/NativeHandle.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
namespace c3=aidl::android::hardware::graphics::composer3;
namespace common=aidl::android::hardware::graphics::common;
static void need(bool ok,const char *s){if(!ok){fprintf(stderr,"A6L_GRAPHICS_CLIENT_FAIL %s\n",s);exit(1);}}
class Callback final:public c3::BnComposerCallback {
 public:
    std::atomic<int64_t> display{-1};
    ndk::ScopedAStatus onHotplug(int64_t d,bool connected) override {if(connected)display=d;return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onHotplugEvent(int64_t d,common::DisplayHotplugEvent e) override {if(e==common::DisplayHotplugEvent::CONNECTED)display=d;return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onRefresh(int64_t) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onSeamlessPossible(int64_t) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onVsync(int64_t,int64_t,int32_t) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onVsyncPeriodTimingChanged(int64_t,const c3::VsyncPeriodChangeTimeline&) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onVsyncIdle(int64_t) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onRefreshRateChangedDebug(const c3::RefreshRateChangedDebugData&) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onHdcpLevelsChanged(int64_t,const aidl::android::hardware::drm::HdcpLevels&) override{return ndk::ScopedAStatus::ok();}
};
static ndk::SpAIBinder service(const char *name) {
    for(int i=0;i<100;i++){ndk::SpAIBinder b(AServiceManager_checkService(name));if(b.get())return b;usleep(100000);}
    need(false,name);return {};
}

static void status(const ndk::ScopedAStatus &s,const char *step) {
    if(!s.isOk())fprintf(stderr,"A6L_PRESENT_STATUS %s: %s\n",step,s.getDescription().c_str());
    need(s.isOk(),step);
}
static c3::Buffer transport(buffer_handle_t h,int slot) {
    c3::Buffer b;b.slot=slot;aidl::android::hardware::common::NativeHandle n;
    for(int i=0;i<h->numFds;i++){int fd=dup(h->data[i]);need(fd>=0,"duplicate buffer fd");n.fds.emplace_back(fd);}
    for(int i=0;i<h->numInts;i++)n.ints.push_back(h->data[h->numFds+i]);
    b.handle=std::move(n);return b;
}
static void commands(const std::shared_ptr<c3::IComposerClient> &client,c3::DisplayCommand cmd,bool present) {
    printf("A6L_PRESENT_COMMAND validate=%d accept=%d present=%d\n",cmd.validateDisplay,cmd.acceptDisplayChanges,cmd.presentDisplay);
    std::vector<c3::DisplayCommand> in;in.push_back(std::move(cmd));
    std::vector<c3::CommandResultPayload> out;status(client->executeCommands(in,&out),"execute commands");
    bool fence_seen=false;
    for(const auto &r:out){
        using R=c3::CommandResultPayload;
        if(r.getTag()==R::Tag::error){auto &e=r.get<R::Tag::error>();fprintf(stderr,"A6L_PRESENT_COMMAND_ERROR index=%d code=%d\n",e.commandIndex,e.errorCode);need(false,"command error");}
        if(r.getTag()==R::Tag::changedCompositionTypes){auto &v=r.get<R::Tag::changedCompositionTypes>();need(v.layers.empty(),"unexpected composition change");}
        if(r.getTag()==R::Tag::displayRequest){auto &v=r.get<R::Tag::displayRequest>();need((v.mask&~c3::DisplayRequest::FLIP_CLIENT_TARGET)==0&&v.layerRequests.empty(),"unexpected display request");}
        if(r.getTag()==R::Tag::presentFence){auto &v=r.get<R::Tag::presentFence>();fence_seen=true;
            if(v.fence.get()>=0){struct pollfd p={v.fence.get(),POLLIN,0};need(poll(&p,1,3000)==1&&(p.revents&POLLIN),"present fence signaled");}
        }
    }
    need(!present||fence_seen,"present result");
}
static void present_frames(const std::shared_ptr<c3::IComposerClient> &client,int64_t display,int32_t config) {
    status(client->setActiveConfig(display,config),"active display config");
    status(client->setPowerMode(display,c3::PowerMode::ON),"display on");
    status(client->setClientTargetSlotCount(display,2),"client target slots");
    int64_t layer=-1;status(client->createLayer(display,2,&layer),"create client layer");
    auto &alloc=android::GraphicBufferAllocator::get();auto &mapper=android::GraphicBufferMapper::get();
    buffer_handle_t handles[2]={};uint32_t strides[2]={};
    // The composer uses the opaque XRGB view of this final BGRA client target.
    for(int i=0;i<2;i++)need(alloc.allocate(1080,2340,HAL_PIXEL_FORMAT_BGRA_8888,1,
        GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER|GRALLOC_USAGE_HW_TEXTURE|GRALLOC_USAGE_SW_WRITE_OFTEN,
        &handles[i],&strides[i],"A6L-V43")==0&&handles[i]&&strides[i]>=1080,"present buffer allocate");
    for(unsigned frame=0;frame<4;frame++){
        unsigned slot=frame%2;void *addr=nullptr;
        need(mapper.lock(handles[slot],GRALLOC_USAGE_SW_WRITE_OFTEN,android::Rect(1080,2340),&addr)==0&&addr,"present paint lock");
        const uint32_t bgra[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff,0xff000000};
        for(unsigned y=0;y<2340;y++)for(unsigned x=0;x<1080;x++){
            uint32_t color=bgra[x*5/1080];
            if(y>=1170){unsigned v=x*255/1079;color=0xff000000|v|(v<<8)|(v<<16);}
            if(y>=1800&&y<1900&&x>=frame*200&&x<frame*200+150)color=0xffffffff;
            static_cast<uint32_t*>(addr)[y*strides[slot]+x]=color;
        }
        need(mapper.unlock(handles[slot])==0,"present paint unlock");
        c3::DisplayCommand cmd;cmd.display=display;
        c3::LayerCommand lc;lc.layer=layer;lc.composition=c3::ParcelableComposition{.composition=c3::Composition::CLIENT};
        lc.displayFrame=common::Rect{.left=0,.top=0,.right=1080,.bottom=2340};
        lc.sourceCrop=common::FRect{.left=0,.top=0,.right=1080,.bottom=2340};
        lc.z=c3::ZOrder{.z=0};cmd.layers.push_back(std::move(lc));
        c3::ClientTarget target;target.buffer=transport(handles[slot],slot);target.dataspace=common::Dataspace::SRGB;
        cmd.clientTarget=std::move(target);cmd.validateDisplay=true;commands(client,std::move(cmd),false);
        c3::DisplayCommand accept;accept.display=display;accept.acceptDisplayChanges=true;commands(client,std::move(accept),false);
        c3::DisplayCommand show;show.display=display;show.presentDisplay=true;commands(client,std::move(show),true);
        printf("A6L_PRESENT_FRAME frame=%u buffers=2\n",frame);
        if(frame==3){puts("A6L_PRESENT_VISIBLE red_green_blue_white_black_top_gray_gradient_bottom hold=8s");sleep(8);}
        else usleep(350000);
    }
    status(client->destroyLayer(display,layer),"destroy layer");
    for(auto h:handles)need(alloc.free(h)==0,"free present buffer");
    puts("A6L_PRESENT_PASS frames=4 buffers=2 commands=checked fences=checked");
}

int main() {
    setbuf(stdout,nullptr);alarm(40);ABinderProcess_setThreadPoolMaxThreadCount(2);ABinderProcess_startThreadPool();
    const char *allocator="android.hardware.graphics.allocator.IAllocator/default";
    auto a=service(allocator);need(AServiceManager_isDeclared(allocator),"allocator VINTF declaration");
    const char *composer="android.hardware.graphics.composer3.IComposer/default";
    auto raw=service(composer);need(AServiceManager_isDeclared(composer),"composer VINTF declaration");
    puts("A6L_GRAPHICS_SERVICES_DISCOVERED allocator=1 composer=1 vintf=1");
    auto &mapper=android::GraphicBufferMapper::get();
    printf("A6L_GRAPHICS_MAPPER version=%d\n",static_cast<int>(mapper.getMapperVersion()));
    need(static_cast<int>(mapper.getMapperVersion())==5,"stable mapper 5");
    auto &alloc=android::GraphicBufferAllocator::get();buffer_handle_t h=nullptr;uint32_t stride=0;
    need(alloc.allocate(1080,2340,HAL_PIXEL_FORMAT_RGBA_8888,1,GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER|GRALLOC_USAGE_HW_TEXTURE|GRALLOC_USAGE_SW_READ_OFTEN|GRALLOC_USAGE_SW_WRITE_OFTEN,&h,&stride,"A6L-V42")==0&&h&&stride>=1080,"AIDL allocate/import");
    uint64_t w=0,ht=0;need(mapper.getWidth(h,&w)==0&&mapper.getHeight(h,&ht)==0&&w==1080&&ht==2340,"mapper metadata");
    void *addr=nullptr;need(mapper.lock(h,GRALLOC_USAGE_SW_WRITE_OFTEN,android::Rect(1080,2340),&addr)==0&&addr,"mapper write lock");
    for(unsigned y=0;y<2340;y++)for(unsigned x=0;x<1080;x++)static_cast<uint32_t*>(addr)[y*stride+x]=0xff123400U^(x*257U)^(y*13U);
    need(mapper.unlock(h)==0,"mapper write unlock");
    need(mapper.lock(h,GRALLOC_USAGE_SW_READ_OFTEN,android::Rect(1080,2340),&addr)==0&&addr,"mapper read lock");
    for(unsigned y=0;y<2340;y++)for(unsigned x=0;x<1080;x++)need(static_cast<uint32_t*>(addr)[y*stride+x]==(0xff123400U^(x*257U)^(y*13U)),"mapper pixel check");
    need(mapper.unlock(h)==0,"mapper read unlock");need(alloc.free(h)==0,"buffer free");
    puts("A6L_GRAPHICS_ALLOCATOR_PASS binder=1 mapper5=1 metadata=1 pixels=2527200");
    auto hwc=c3::IComposer::fromBinder(raw);need(hwc!=nullptr,"composer interface");
    int32_t version=0;need(hwc->getInterfaceVersion(&version).isOk()&&version==5,"composer version");
    std::shared_ptr<c3::IComposerClient> client;need(hwc->createClient(&client).isOk()&&client,"composer client");
    auto cb=ndk::SharedRefBase::make<Callback>();need(client->registerCallback(cb).isOk(),"composer callback");
    for(int i=0;i<50&&cb->display<0;i++)usleep(100000);
    need(cb->display>=0,"physical display hotplug");std::vector<c3::DisplayConfiguration> configs;
    need(client->getDisplayConfigurations(cb->display,0,&configs).isOk(),"display configurations");
    int32_t config_id=-1;bool exact=false;for(const auto &c:configs){printf("A6L_GRAPHICS_DISPLAY id=%lld config=%d width=%d height=%d vsync_ns=%d\n",(long long)cb->display.load(),c.configId,c.width,c.height,c.vsyncPeriod);if(c.width==1080&&c.height==2340){exact=true;config_id=c.configId;}}
    need(exact,"native display size");present_frames(client,cb->display,config_id);client.reset();hwc.reset();
    puts("A6L_GRAPHICS_CLIENT_PASS allocator_aidl=1 mapper5=1 composer5=1 native_display=1");return 0;
}
