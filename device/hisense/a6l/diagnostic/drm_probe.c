/* V40: fixed simpleDRM LCD only; no persistent storage access. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define W 1080U
#define H 2340U
static void check(int ok,const char *what) {
    if(!ok){fprintf(stderr,"A6L_DRM_FAIL %s errno=%d %s\n",what,errno,strerror(errno));exit(1);}
}
static uint32_t property(int fd,uint32_t object,uint32_t type,const char *name) {
    drmModeObjectProperties *ps=drmModeObjectGetProperties(fd,object,type);check(ps!=NULL,"object properties");
    uint32_t id=0;
    for(uint32_t i=0;i<ps->count_props;i++) {
        drmModePropertyRes *p=drmModeGetProperty(fd,ps->props[i]);check(p!=NULL,"property definition");
        if(!strcmp(p->name,name))id=p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(ps);check(id!=0,name);return id;
}
static void add(int fd,drmModeAtomicReq *req,uint32_t obj,uint32_t type,const char *name,uint64_t value) {
    check(drmModeAtomicAddProperty(req,obj,property(fd,obj,type,name),value)>=0,name);
}
struct buffer {uint32_t handle,fb,pitch;uint64_t size;void *map;};
static struct buffer buffer(int fd) {
    struct drm_mode_create_dumb c={.width=W,.height=H,.bpp=32};
    check(!drmIoctl(fd,DRM_IOCTL_MODE_CREATE_DUMB,&c),"create dumb buffer");
    check(c.pitch>=W*4&&c.size>=(uint64_t)c.pitch*H&&c.size<32*1024*1024,"buffer bounds");
    uint32_t handles[4]={c.handle},pitches[4]={c.pitch},offsets[4]={0},fb=0;
    check(!drmModeAddFB2(fd,W,H,DRM_FORMAT_XRGB8888,handles,pitches,offsets,&fb,0),"add framebuffer");
    struct drm_mode_map_dumb m={.handle=c.handle};check(!drmIoctl(fd,DRM_IOCTL_MODE_MAP_DUMB,&m),"map dumb offset");
    void *map=mmap(NULL,c.size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,m.offset);check(map!=MAP_FAILED,"map buffer");
    return (struct buffer){c.handle,fb,c.pitch,c.size,map};
}
static void paint(struct buffer *b,unsigned frame) {
    const uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff,0xff000000};
    for(unsigned y=0;y<H;y++) {
        uint32_t *row=(uint32_t *)((char *)b->map+y*b->pitch);
        for(unsigned x=0;x<W;x++) {
            uint32_t c=colors[x*5/W];
            if(y>H/2)c=0xff000000|((x*255/W)<<16)|((y*255/H)<<8)|0x40;
            if(y>H/3&&y<H/3+100&&x>frame*100&&x<frame*100+100)c=0xffffffff;
            row[x]=c;
        }
    }
    __sync_synchronize();
}
int main(void) {
    setbuf(stdout,NULL);alarm(45);
    char prop[PROP_VALUE_MAX];__system_property_get("ro.a6l.ramdiag",prop);
    check(!strcmp(prop,"v38")&&getuid()==0,"V38 root environment");
    struct statfs fs;check(!statfs("/tmp",&fs)&&(unsigned long)fs.f_type==0x01021994,"tmpfs required");
    check(!unshare(CLONE_NEWNS),"private mount namespace");
    check(!mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL),"private mount propagation");
    check(!mkdir("/tmp/a6l-v40/work",0700),"fresh private work directory");
    check(!mount("tmpfs","/tmp/a6l-v40/work","tmpfs",MS_NOSUID,"size=1m,mode=0700"),"device-enabled private tmpfs");
    DIR *d=opendir("/sys/class/drm");check(d!=NULL,"DRM sysfs");
    struct dirent *ent;unsigned major=0,minor=0;char path[256];int cards=0;
    while((ent=readdir(d))) {
        unsigned idx;char extra;
        if(sscanf(ent->d_name,"card%u%c",&idx,&extra)!=1)continue;
        snprintf(path,sizeof(path),"/sys/class/drm/card%u/dev",idx);
        FILE *f=fopen(path,"r");check(f!=NULL,"DRM dev numbers");
        check(fscanf(f,"%u:%u",&major,&minor)==2,"DRM dev parse");fclose(f);cards++;
    }
    closedir(d);check(cards==1&&major==226&&minor<64,"exactly one primary DRM device");
    const char *node="/tmp/a6l-v40/work/drm-card";
    check(!mknod(node,S_IFCHR|0600,makedev(major,minor)),"fresh DRM device node");
    int fd=open(node,O_RDWR|O_CLOEXEC|O_NOFOLLOW);check(fd>=0,"open DRM");
    drmVersion *version=drmGetVersion(fd);check(version&&!strcmp(version->name,"simpledrm"),"simpledrm identity");
    printf("A6L_DRM_DRIVER name=%s major=%u minor=%u\n",version->name,major,minor);drmFreeVersion(version);
    check(!drmSetClientCap(fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1),"universal planes");
    check(!drmSetClientCap(fd,DRM_CLIENT_CAP_ATOMIC,1),"atomic capability");
    check(drmIsMaster(fd)||!drmSetMaster(fd),"DRM master");
    drmModeRes *res=drmModeGetResources(fd);check(res&&res->count_connectors==1&&res->count_crtcs==1,"fixed display topology");
    drmModeConnector *con=drmModeGetConnector(fd,res->connectors[0]);
    check(con&&con->connection==DRM_MODE_CONNECTED&&con->count_modes==1,"connected fixed LCD");
    check(con->modes[0].hdisplay==W&&con->modes[0].vdisplay==H,"LCD dimensions");
    drmModeCrtc *old=drmModeGetCrtc(fd,res->crtcs[0]);check(old!=NULL,"save old CRTC");
    drmModePlaneRes *planes=drmModeGetPlaneResources(fd);check(planes&&planes->count_planes==1,"single primary plane");
    uint32_t plane=planes->planes[0],crtc=res->crtcs[0],connector=con->connector_id,blob=0;
    check(!drmModeCreatePropertyBlob(fd,&con->modes[0],sizeof(con->modes[0]),&blob),"mode blob");
    struct buffer b[2]={buffer(fd),buffer(fd)};paint(&b[0],0);paint(&b[1],1);
    printf("A6L_DRM_BUFFERS count=2 width=%u height=%u pitch=%u\n",W,H,b[0].pitch);
    int prime=-1;check(!drmPrimeHandleToFD(fd,b[0].handle,DRM_CLOEXEC|DRM_RDWR,&prime),"PRIME export");
    int other=open(node,O_RDWR|O_CLOEXEC);check(other>=0,"second DRM client");uint32_t imported=0;
    check(!drmPrimeFDToHandle(other,prime,&imported),"PRIME import");
    struct drm_gem_close close_import={.handle=imported};check(!drmIoctl(other,DRM_IOCTL_GEM_CLOSE,&close_import),"close imported buffer");
    close(other);close(prime);puts("A6L_DRM_PRIME export=1 import=1");
    drmModeAtomicReq *req=drmModeAtomicAlloc();check(req!=NULL,"atomic request");
    add(fd,req,connector,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",crtc);
    add(fd,req,crtc,DRM_MODE_OBJECT_CRTC,"MODE_ID",blob);add(fd,req,crtc,DRM_MODE_OBJECT_CRTC,"ACTIVE",1);
    add(fd,req,plane,DRM_MODE_OBJECT_PLANE,"FB_ID",b[0].fb);add(fd,req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_ID",crtc);
    const char *names[]={"SRC_X","SRC_Y","SRC_W","SRC_H","CRTC_X","CRTC_Y","CRTC_W","CRTC_H"};
    const uint64_t vals[]={0,0,W<<16,H<<16,0,0,W,H};
    for(unsigned i=0;i<8;i++)add(fd,req,plane,DRM_MODE_OBJECT_PLANE,names[i],vals[i]);
    check(!drmModeAtomicCommit(fd,req,DRM_MODE_ATOMIC_TEST_ONLY|DRM_MODE_ATOMIC_ALLOW_MODESET,NULL),"atomic test-only");
    check(!drmModeAtomicCommit(fd,req,DRM_MODE_ATOMIC_ALLOW_MODESET,NULL),"atomic modeset");drmModeAtomicFree(req);
    for(unsigned frame=1;frame<=8;frame++) {
        usleep(300000);paint(&b[frame%2],frame);
        req=drmModeAtomicAlloc();check(req!=NULL,"flip request");
        add(fd,req,plane,DRM_MODE_OBJECT_PLANE,"FB_ID",b[frame%2].fb);
        check(!drmModeAtomicCommit(fd,req,0,NULL),"atomic framebuffer update");drmModeAtomicFree(req);
    }
    puts("A6L_DRM_PASS atomic=1 buffers=2 updates=8 prime_export=1 prime_import=1");
    puts("A6L_DRM_VISIBLE red_green_blue_white_black_top_gradient_bottom_hold=12s");sleep(12);
    if(old->mode_valid)check(!drmModeSetCrtc(fd,old->crtc_id,old->buffer_id,old->x,old->y,&connector,1,&old->mode),"restore previous CRTC");
    else check(!drmModeSetCrtc(fd,crtc,0,0,0,NULL,0,NULL),"disable test CRTC");
    for(unsigned i=0;i<2;i++){check(!drmModeRmFB(fd,b[i].fb),"remove test framebuffer");munmap(b[i].map,b[i].size);struct drm_mode_destroy_dumb x={.handle=b[i].handle};check(!drmIoctl(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&x),"destroy test buffer");}
    drmModeDestroyPropertyBlob(fd,blob);drmModeFreeCrtc(old);drmModeFreePlaneResources(planes);drmModeFreeConnector(con);drmModeFreeResources(res);close(fd);unlink(node);
    check(!umount("/tmp/a6l-v40/work"),"unmount private work");
    puts("A6L_DRM_CLEANUP_PASS");return 0;
}
