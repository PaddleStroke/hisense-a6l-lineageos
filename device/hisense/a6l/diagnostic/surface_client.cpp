/* Exercise real SurfaceFlinger composition through native colour layers. */
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <gui/Surface.h>
#include <ui/Rect.h>
#include <ui/LayerStack.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
using namespace android;
static void need(bool ok,const char *what){if(!ok){fprintf(stderr,"A6L_SURFACE_FAIL %s\n",what);exit(1);}}
int main(){
    setbuf(stdout,nullptr);alarm(75);ProcessState::self()->startThreadPool();
    auto sm=defaultServiceManager();sp<IBinder> sf;
    for(int i=0;i<400&&!sf;i++){sf=sm->checkService(String16("SurfaceFlinger"));if(!sf)usleep(100000);}
    need(sf!=nullptr,"SurfaceFlinger service");
    auto ids=SurfaceComposerClient::getPhysicalDisplayIds();need(ids.size()==1,"one physical display");
    auto display=SurfaceComposerClient::getPhysicalDisplayToken(ids[0]);need(display!=nullptr,"display token");
    auto client=sp<SurfaceComposerClient>::make();need(client->initCheck()==OK,"client connection");
    SurfaceComposerClient::setDisplayPowerMode(display,2);
    // Full bootFinished waits for framework WindowManager; this is an early-boot client.
    SurfaceComposerClient::Transaction transaction;
    ui::LayerStack stack{44};transaction.setDisplayLayerStack(display,stack);
    transaction.setDisplayProjection(display,ui::ROTATION_0,Rect(1080,2340),Rect(1080,2340));
    // SurfaceFlinger leaves BOOTLOADER only after latching a real buffer.
    // Effect-only colour layers cannot trigger that transition themselves.
    auto first=client->createSurface(String8("A6L first buffer"),16,16,PIXEL_FORMAT_RGBA_8888,0);
    need(first!=nullptr&&first->isValid(),"first buffer surface");
    transaction.setLayerStack(first,stack).setLayer(first,0).show(first);
    sp<SurfaceControl> layers[4];
    const half3 colors[4]={{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    for(int i=0;i<4;i++){
        layers[i]=client->createSurface(String8("A6L SurfaceFlinger test"),540,1170,PIXEL_FORMAT_RGBA_8888,0x00020000);
        need(layers[i]!=nullptr&&layers[i]->isValid(),"colour layer");
        transaction.setLayerStack(layers[i],stack).setLayer(layers[i],100+i)
          .setColor(layers[i],colors[i]).setCrop(layers[i],Rect(540,1170))
          .setPosition(layers[i],(i%2)*540,(i/2)*1170).show(layers[i]);
    }
    need(transaction.apply(true)==OK,"synchronous layer transaction");
    auto surface=first->getSurface(); ANativeWindow_Buffer buffer{};
    need(surface->lock(&buffer,nullptr)==OK,"first buffer lock");
    for(int y=0;y<buffer.height;y++)for(int x=0;x<buffer.width;x++)
        static_cast<uint32_t*>(buffer.bits)[y*buffer.stride+x]=0xff000000;
    need(surface->unlockAndPost()==OK,"first buffer post");
    puts("A6L_SURFACE_SUBMITTED layers=4 width=1080 height=2340");
    sleep(3);
    int dump=open("/logs/surfaceflinger.dump",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    need(dump>=0,"diagnostic dump file");
    need(sf->dump(dump,Vector<String16>{})==OK,"SurfaceFlinger diagnostic dump");close(dump);
    puts("A6L_SURFACE_VISIBLE quadrants=red_green_blue_white hold=8s");sleep(8);
    SurfaceComposerClient::Transaction remove;for(auto &layer:layers)remove.hide(layer);
    remove.hide(first);
    need(remove.apply(true)==OK,"hide layers");
    puts("A6L_SURFACE_PASS real_surfaceflinger=1 layers=4 transaction=checked");return 0;
}
