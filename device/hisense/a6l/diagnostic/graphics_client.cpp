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
    bool exact=false;for(const auto &c:configs){printf("A6L_GRAPHICS_DISPLAY id=%lld config=%d width=%d height=%d vsync_ns=%d\n",(long long)cb->display.load(),c.configId,c.width,c.height,c.vsyncPeriod);exact|=c.width==1080&&c.height==2340;}
    need(exact,"native display size");client.reset();hwc.reset();
    puts("A6L_GRAPHICS_CLIENT_PASS allocator_aidl=1 mapper5=1 composer5=1 native_display=1");return 0;
}
