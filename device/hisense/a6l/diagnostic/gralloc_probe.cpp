/* Real minigbm Android allocator, metadata and cross-process native-handle test.
 * No storage access and no display mode changes. Runs in a private mount namespace.
 */
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>
#include <hardware/gralloc.h>
#include <xf86drm.h>
#include "cros_gralloc_driver.h"

static constexpr unsigned WIDTH=1080,HEIGHT=2340;
static void check(bool ok,const char *what) {
    if(!ok){fprintf(stderr,"A6L_GRALLOC_FAIL %s errno=%d %s\n",what,errno,strerror(errno));exit(1);}
}
static uint32_t pixel(unsigned x,unsigned y){return 0xff123400U^(x*257U)^(y*13U);}
static void close_handle(native_handle_t *h){check(!native_handle_close(h),"close native handle");check(!native_handle_delete(h),"delete native handle");}
struct message {int fds,ints;int data[128];};
static void send_handle(int sock,native_handle_t *h) {
    check(h->numFds>0&&h->numFds<=5&&h->numInts<=128,"native handle bounds");
    message m{};m.fds=h->numFds;m.ints=h->numInts;memcpy(m.data,h->data+h->numFds,h->numInts*sizeof(int));
    char control[CMSG_SPACE(5*sizeof(int))]={};iovec iov{&m,sizeof(m)};msghdr msg{};
    msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=CMSG_SPACE(h->numFds*sizeof(int));
    auto *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(h->numFds*sizeof(int));memcpy(CMSG_DATA(c),h->data,h->numFds*sizeof(int));
    check(sendmsg(sock,&msg,0)==sizeof(m),"send native handle");
}
static native_handle_t *receive_handle(int sock) {
    message m{};char control[CMSG_SPACE(5*sizeof(int))]={};iovec iov{&m,sizeof(m)};msghdr msg{};
    msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
    check(recvmsg(sock,&msg,MSG_CMSG_CLOEXEC)==sizeof(m)&&!(msg.msg_flags&(MSG_CTRUNC|MSG_TRUNC)),"receive native handle");
    check(m.fds>0&&m.fds<=5&&m.ints>=0&&m.ints<=128,"received handle bounds");
    auto *c=CMSG_FIRSTHDR(&msg);check(c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len==CMSG_LEN(m.fds*sizeof(int)),"SCM_RIGHTS");
    auto *h=native_handle_create(m.fds,m.ints);check(h!=nullptr,"allocate native handle");
    memcpy(h->data,CMSG_DATA(c),m.fds*sizeof(int));memcpy(h->data+m.fds,m.data,m.ints*sizeof(int));return h;
}
static void unlock(const std::shared_ptr<cros_gralloc_driver>&drv,native_handle_t *h) {
    int fence=-1;check(!drv->unlock(h,&fence),"unlock allocator buffer");if(fence>=0)close(fence);
}
static int child(int sock) {
    alarm(12);prctl(PR_SET_PDEATHSIG,SIGKILL);check(getppid()!=1,"parent alive");
    auto *h=receive_handle(sock);auto driver=cros_gralloc_driver::get_instance();check(driver!=nullptr,"child allocator driver");
    check(!driver->retain(h),"cross-process allocator import");auto c=cros_gralloc_convert_handle(h);check(c!=nullptr,"imported metadata handle");
    check(c->width==WIDTH&&c->height==HEIGHT&&c->num_planes==1&&c->reserved_region_size>0,"imported dimensions/metadata");
    rectangle rect{0,0,WIDTH,HEIGHT};uint8_t *addr[DRV_MAX_PLANES]={};
    check(!driver->lock(h,-1,false,&rect,BO_MAP_READ,addr)&&addr[0],"child map");
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++) {
        uint32_t v;memcpy(&v,addr[0]+y*c->strides[0]+x*4,4);check(v==pixel(x,y),"cross-process pixel coherence");
    }
    unlock(driver,h);check(!driver->release(h),"child release");close_handle(h);close(sock);
    puts("A6L_GRALLOC_CHILD_PASS full_buffer_pixels=2527200 metadata=1");return 0;
}
static void test_format(int format,const char *name) {
    auto driver=cros_gralloc_driver::get_instance();check(driver!=nullptr,"allocator driver");
    cros_gralloc_buffer_descriptor desc{};desc.width=WIDTH;desc.height=HEIGHT;desc.droid_format=format;
    desc.droid_usage=GRALLOC_USAGE_HW_COMPOSER|GRALLOC_USAGE_HW_RENDER|GRALLOC_USAGE_HW_TEXTURE|GRALLOC_USAGE_SW_READ_OFTEN|GRALLOC_USAGE_SW_WRITE_OFTEN;
    desc.drm_format=cros_gralloc_convert_format(format);desc.use_flags=cros_gralloc_convert_usage(desc.droid_usage);
    desc.enable_metadata_fd=true;desc.client_metadata_size=256;desc.name=std::string("A6L-V41-")+name;
    check(driver->is_supported(&desc),"Android format/usage support");native_handle_t *h=nullptr;
    check(!driver->allocate(&desc,&h)&&h,"Android allocator allocate");auto c=cros_gralloc_convert_handle(h);check(c!=nullptr,"allocator native handle");
    check(c->width==WIDTH&&c->height==HEIGHT&&c->num_planes==1&&c->strides[0]>=WIDTH*4&&c->reserved_region_size>=256,"native dimensions and metadata");
    bool metadata=false;driver->with_buffer(c,[&](cros_gralloc_buffer *b){std::optional<std::string> label;check(!b->get_name(&label)&&label&&*label==desc.name,"buffer metadata name");metadata=true;});check(metadata,"metadata callback");
    rectangle rect{0,0,WIDTH,HEIGHT};uint8_t *addr[DRV_MAX_PLANES]={};
    check(!driver->lock(h,-1,false,&rect,BO_MAP_WRITE,addr)&&addr[0],"CPU-write map");
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++){uint32_t v=pixel(x,y);memcpy(addr[0]+y*c->strides[0]+x*4,&v,4);}
    unlock(driver,h);
    int sv[2];check(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,sv),"handle transport socket");
    pid_t pid=fork();check(pid>=0,"fork importer");
    if(!pid){close(sv[0]);char fd[24];snprintf(fd,sizeof(fd),"%d",sv[1]);execl("/proc/self/exe","a6l_gralloc_probe","--child",fd,nullptr);_exit(127);}
    close(sv[1]);send_handle(sv[0],h);close(sv[0]);int status=0;
    check(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh-process import test");
    check(!driver->release(h),"release allocated buffer");close_handle(h);
    printf("A6L_GRALLOC_FORMAT_PASS name=%s width=%u height=%u metadata=1 cross_process=1\n",name,WIDTH,HEIGHT);
}
int main(int argc,char **argv) {
    setbuf(stdout,nullptr);alarm(40);char prop[PROP_VALUE_MAX];__system_property_get("ro.a6l.ramdiag",prop);
    check(!strcmp(prop,"v38")&&getuid()==0,"V38 root environment");
    if(argc==3&&!strcmp(argv[1],"--child")){char *end=nullptr;long n=strtol(argv[2],&end,10);check(end&&!*end&&n>=0&&n<1024,"child fd");return child(n);}
    check(argc==1,"fixed probe invocation");
    struct statfs fs;check(!statfs("/tmp",&fs)&&(unsigned long)fs.f_type==0x01021994,"RAM environment");
    FILE *f=fopen("/sys/class/drm/card0/dev","r");unsigned major=0,minor=0;check(f&&fscanf(f,"%u:%u",&major,&minor)==2,"DRM device identity");fclose(f);check(major==226&&minor==0,"fixed primary node");
    check(!unshare(CLONE_NEWNS),"private mount namespace");check(!mount(nullptr,"/",nullptr,MS_REC|MS_PRIVATE,nullptr),"private propagation");
    bool made=!mkdir("/dev/dri",0755);check(made||errno==EEXIST,"DRM directory");struct stat st;check(!lstat("/dev/dri",&st)&&S_ISDIR(st.st_mode),"DRM directory type");
    check(!mount("tmpfs","/dev/dri","tmpfs",MS_NOSUID|MS_NOEXEC,"size=1m,mode=0755"),"private DRM device tmpfs");
    check(!mknod("/dev/dri/card0",S_IFCHR|0600,makedev(major,minor)),"private DRM node");
    int fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);check(fd>=0,"open simpleDRM");auto version=drmGetVersion(fd);check(version&&!strcmp(version->name,"simpledrm"),"simpleDRM driver");drmFreeVersion(version);close(fd);
    test_format(HAL_PIXEL_FORMAT_RGBA_8888,"RGBA8888");test_format(HAL_PIXEL_FORMAT_RGBX_8888,"RGBX8888");
    // The allocator singleton keeps its device FD until process exit. Detach
    // only this private mount; normal process teardown closes that last FD.
    check(!umount2("/dev/dri",MNT_DETACH),"private DRM detach");if(made)check(!rmdir("/dev/dri"),"remove temporary DRM directory");
    puts("A6L_GRALLOC_PASS formats=2 metadata=1 cross_process_import=1 pixel_coherence=1");return 0;
}
