"""Create the next bounded presentation probe from the preserved V42 sources."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
d=root/'device/hisense/a6l/diagnostic'
for name in ['present_services.c','present_client.cpp']:
    assert not (d/name).exists()
supervisor=(d/'graphics_services.c').read_text().replace('/tmp/a6l-v42','/tmp/a6l-v43').replace('a6l-v42','a6l-v43')
(d/'present_services.c').write_text(supervisor)
client=(d/'graphics_client.cpp').read_text()
client=client.replace('#include <atomic>','#include <poll.h>\n#include <aidl/android/hardware/common/NativeHandle.h>\n#include <atomic>')
helper=r'''
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
    std::vector<c3::DisplayCommand> in;in.push_back(std::move(cmd));
    std::vector<c3::CommandResultPayload> out;status(client->executeCommands(in,&out),"execute commands");
    bool fence_seen=false;
    for(const auto &r:out){
        using R=c3::CommandResultPayload;
        if(r.getTag()==R::Tag::error){auto &e=r.get<R::Tag::error>();fprintf(stderr,"A6L_PRESENT_COMMAND_ERROR index=%d code=%d\n",e.commandIndex,e.errorCode);need(false,"command error");}
        if(r.getTag()==R::Tag::changedCompositionTypes){auto &v=r.get<R::Tag::changedCompositionTypes>();need(v.layers.empty(),"unexpected composition change");}
        if(r.getTag()==R::Tag::displayRequest){auto &v=r.get<R::Tag::displayRequest>();need(v.mask==0&&v.layerRequests.empty(),"unexpected display request");}
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
    for(int i=0;i<2;i++)need(alloc.allocate(1080,2340,HAL_PIXEL_FORMAT_RGBA_8888,1,
        GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER|GRALLOC_USAGE_HW_TEXTURE|GRALLOC_USAGE_SW_WRITE_OFTEN,
        &handles[i],&strides[i],"A6L-V43")==0&&handles[i]&&strides[i]>=1080,"present buffer allocate");
    for(unsigned frame=0;frame<4;frame++){
        unsigned slot=frame%2;void *addr=nullptr;
        need(mapper.lock(handles[slot],GRALLOC_USAGE_SW_WRITE_OFTEN,android::Rect(1080,2340),&addr)==0&&addr,"present paint lock");
        const uint32_t rgba[]={0xff0000ff,0xff00ff00,0xffff0000,0xffffffff,0xff000000};
        for(unsigned y=0;y<2340;y++)for(unsigned x=0;x<1080;x++){
            uint32_t color=rgba[x*5/1080];
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
'''
client=client.replace('int main() {',helper+'\nint main() {')
client=client.replace('bool exact=false;','int32_t config_id=-1;bool exact=false;')
client=client.replace('exact|=c.width==1080&&c.height==2340;','if(c.width==1080&&c.height==2340){exact=true;config_id=c.configId;}')
client=client.replace('need(exact,"native display size");client.reset();hwc.reset();','need(exact,"native display size");present_frames(client,cb->display,config_id);client.reset();hwc.reset();')
(d/'present_client.cpp').write_text(client)
bp=root/'device/hisense/a6l/Android.bp'
base=bp.read_text();cut=base.index('cc_binary {\n    name: "a6l_gralloc_probe"')
addition=base[:cut].replace('a6l_graphics_services','a6l_present_services').replace('a6l_graphics_client','a6l_present_client').replace('diagnostic/graphics_services.c','diagnostic/present_services.c').replace('diagnostic/graphics_client.cpp','diagnostic/present_client.cpp')
bp.write_text(addition+base)
build=(root/'tools/build-android-graphics-v42.sh').read_text().replace('graphics-v42','present-v43').replace('diagnostic/graphics_','diagnostic/present_').replace('a6l_graphics_services','a6l_present_services').replace('a6l_graphics_client','a6l_present_client')
(root/'tools/build-android-present-v43.sh').write_text(build)
print('V43 presentation sources and build script prepared')
