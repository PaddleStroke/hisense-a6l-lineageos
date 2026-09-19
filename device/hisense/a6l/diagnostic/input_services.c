/* Isolated RAM-only Android graphics service environment. No block devices. */
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <linux/input.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/android/binderfs.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define ROOT "/tmp/a6l-v47/root"
static pid_t children[9];

static void cleanup(void) {
    for(int i=8;i>=0;i--)if(children[i]>0){kill(-children[i],SIGKILL);waitpid(children[i],NULL,0);children[i]=0;}
}
static void timedout(int sig){(void)sig;cleanup();_exit(124);}
static void need(int ok,const char *why){if(!ok){fprintf(stderr,"A6L_GRAPHICS_FAIL %s errno=%d %s\n",why,errno,strerror(errno));exit(1);}}
static void dir(const char *p){need(!mkdir(p,0755)||errno==EEXIST,p);struct stat s;need(!lstat(p,&s)&&S_ISDIR(s.st_mode),p);}
static void bind_ro(const char *src,const char *dst){dir(dst);need(!mount(src,dst,NULL,MS_BIND,NULL),dst);need(!mount(NULL,dst,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY,NULL),"readonly bind");}

/* Expose exactly one known front touch device, never the complete host /dev. */
static void expose_touch(void) {
    DIR *d=opendir("/sys/class/input");need(d!=NULL,"input sysfs");
    struct dirent *e;int found=0;
    dir(ROOT "/dev/input");
    while((e=readdir(d))!=NULL) {
        unsigned number;char tail;
        if(sscanf(e->d_name,"event%u%c",&number,&tail)!=1||number>31)continue;
        char path[256],name[128]={0};
        snprintf(path,sizeof(path),"/sys/class/input/event%u/device/name",number);
        int f=open(path,O_RDONLY|O_CLOEXEC);if(f<0)continue;
        ssize_t n=read(f,name,sizeof(name)-1);close(f);if(n<=0)continue;
        name[strcspn(name,"\r\n")]=0;
        if(strcmp(name,"generic ft5x06 (8d)")&&strcmp(name,"A6L Input Fixture"))continue;
        snprintf(path,sizeof(path),"/sys/class/input/event%u/dev",number);
        FILE *info=fopen(path,"r");unsigned maj=0,min=0;
        need(info&&fscanf(info,"%u:%u",&maj,&min)==2,"touch dev_t");fclose(info);
        need(maj==13&&min>=64&&min<96,"touch character range");
        snprintf(path,sizeof(path),ROOT "/dev/input/event%u",number);
        need(!mknod(path,S_IFCHR|0600,makedev(maj,min)),"private touch node");
        need(!chown(path,1000,1000),"system touch ownership");
        f=open(path,O_RDONLY|O_CLOEXEC);need(f>=0,"open private touch");
        char actual[128]={0};need(ioctl(f,EVIOCGNAME(sizeof(actual)),actual)>=0&&!strcmp(name,actual),"touch identity");
        close(f);found++;
        printf("A6L_INPUT_NODE name=%s event=%u major=%u minor=%u\n",name,number,maj,min);
    }
    closedir(d);need(found==1,"exactly one front touch device");
}

static void start_logger(void) {
    dir(ROOT "/dev/socket");int s=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);need(s>=0,"log socket");
    struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,ROOT "/dev/socket/logdw");
    need(!bind(s,(struct sockaddr*)&addr,sizeof(addr)),"log socket bind");
    need(!chown(ROOT "/dev/socket/logdw",1000,1000),"system log socket ownership");
    int f=open(ROOT "/logs/android.log",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);need(f>=0,"Android log");
    pid_t p=fork();need(p>=0,"logger fork");
    if(!p){setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(125);
        for(;;){unsigned char b[8192];ssize_t n=recv(s,b,sizeof(b),0);if(n<=0)_exit(0);
            for(ssize_t i=0;i<n;i++)if(b[i]<32&&b[i]!='\n'&&b[i]!='\t')b[i]=' ';
            (void)write(f,b,(size_t)n);(void)write(f,"\n",1);
        }
    }
    children[4]=p;setpgid(p,p);close(s);close(f);
}
#include "private_properties.h"
static pid_t start(int slot,const char *bin,int vendor,const char *logname) {
    char log[256];snprintf(log,sizeof(log),ROOT "/logs/%s.log",logname);
    int fd=open(log,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);need(fd>=0,"fresh child log");
    pid_t p=fork();need(p>=0,"fork child");
    if(!p){setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(125);dup2(fd,1);dup2(fd,2);close(fd);
        if(chroot(ROOT)||chdir("/"))_exit(126);
        if(slot==3){
            if(prctl(PR_SET_KEEPCAPS,1)||setgroups(0,NULL)||setgid(1000)||setuid(1000))_exit(126);
            struct __user_cap_header_struct header={.version=_LINUX_CAPABILITY_VERSION_3,.pid=0};
            struct __user_cap_data_struct caps[2]={{0},{0}};
            const unsigned bit=1u<<(CAP_BLOCK_SUSPEND%32),word=CAP_BLOCK_SUSPEND/32;
            caps[word].effective=caps[word].permitted=caps[word].inheritable=bit;
            if(syscall(__NR_capset,&header,caps)||prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_RAISE,CAP_BLOCK_SUSPEND,0,0))_exit(126);
        }
        /* Credential changes clear PDEATHSIG; restore it for the system client. */
        if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()==1)_exit(125);
        setenv("LD_LIBRARY_PATH",vendor?"/vendor/lib64/hw:/vendor/lib64:/system/lib64":"/system/lib64:/vendor/lib64/hw:/vendor/lib64",1);
        if(slot==8)execl(bin,bin,"--dump-only",NULL);
        else if(slot==0&&access("/system/bin/strace",X_OK)==0)
            execl("/system/bin/strace","strace","-f","-o","/logs/servicemanager.strace",bin,NULL);
        else execl(bin,bin,NULL);
        fprintf(stderr,"exec %s failed: %s\n",bin,strerror(errno));_exit(127);
    }
    children[slot]=p;setpgid(p,p);close(fd);return p;
}
int main(void) {
    umask(022); /* Android init may start this supervisor with umask 077. */
    setbuf(stdout,NULL);atexit(cleanup);signal(SIGALRM,timedout);signal(SIGTERM,timedout);alarm(240);
    char prop[PROP_VALUE_MAX];__system_property_get("ro.a6l.ramdiag",prop);need(getuid()==0&&!strcmp(prop,"v38"),"V38 root only");
    struct statfs fs;need(!statfs(ROOT,&fs)&&(unsigned long)fs.f_type==0x01021994,"RAM root required");
    need(!unshare(CLONE_NEWNS),"private mount namespace");need(!mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL),"private propagation");
    dir(ROOT "/proc");need(!mount("proc",ROOT "/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL),"private proc");
    bind_ro("/sys",ROOT "/sys");
    need(!mount("/sys/fs/selinux",ROOT "/sys/fs/selinux",NULL,MS_BIND,NULL),"SELinux status bind");
    dir(ROOT "/dev");need(!mount("tmpfs",ROOT "/dev","tmpfs",MS_NOSUID,"size=16m,mode=0755"),"private devices");
    need(!mknod(ROOT "/dev/null",S_IFCHR|0666,makedev(1,3)),"null node");
    need(!mknod(ROOT "/dev/zero",S_IFCHR|0666,makedev(1,5)),"zero node");
    need(!mknod(ROOT "/dev/random",S_IFCHR|0666,makedev(1,8)),"random node");
    need(!mknod(ROOT "/dev/urandom",S_IFCHR|0666,makedev(1,9)),"urandom node");
    need(!chmod(ROOT "/dev/null",0666)&&!chmod(ROOT "/dev/zero",0666)&&
         !chmod(ROOT "/dev/random",0666)&&!chmod(ROOT "/dev/urandom",0666),"standard device permissions");
    need(!mknod(ROOT "/dev/kmsg",S_IFCHR|0600,makedev(1,11)),"debug log node");
    expose_touch();
    dir(ROOT "/dev/dri");need(!mknod(ROOT "/dev/dri/card0",S_IFCHR|0600,makedev(226,0)),"DRM node");
    need(!chown(ROOT "/dev/dri/card0",1000,1000),"system DRM ownership");
    dir(ROOT "/dev/binderfs");need(!mount("binder",ROOT "/dev/binderfs","binder",MS_NOSUID|MS_NOEXEC,NULL),"private binderfs");
    int fd=open(ROOT "/dev/binderfs/binder-control",O_RDONLY|O_CLOEXEC);need(fd>=0,"binder control");
    struct binderfs_device b={0};strcpy(b.name,"a6l-v47");need(!ioctl(fd,BINDER_CTL_ADD,&b),"private binder add");strcpy(b.name,"a6l-v47-hw");need(!ioctl(fd,BINDER_CTL_ADD,&b),"private hwbinder add");close(fd);
    need(!symlink("binderfs/a6l-v47",ROOT "/dev/binder"),"binder alias");
    need(!symlink("binderfs/a6l-v47-hw",ROOT "/dev/hwbinder"),"hwbinder alias");
    need(!chown(ROOT "/dev/binderfs/a6l-v47",1000,1000),"system Binder ownership");
    need(!chown(ROOT "/dev/binderfs/a6l-v47-hw",1000,1000),"system HWBinder ownership");
    dir(ROOT "/logs");
    need(!chown(ROOT "/logs",1000,1000),"system log directory");
    start_logger();
    children[5]=start_private_properties();
    puts("A6L_GRAPHICS_NAMESPACE_READY private_binder=1 persistent_mounts=0");
    start(0,"/system/bin/servicemanager",0,"servicemanager");usleep(300000);
    start(7,"/system/bin/hwservicemanager",0,"hwservicemanager");
    start(1,"/vendor/bin/hw/android.hardware.graphics.allocator-service.minigbm",1,"allocator");
    start(2,"/vendor/bin/hw/android.hardware.composer.hwc3-service.drm",1,"composer");
    start(6,"/system/bin/surfaceflinger",0,"surfaceflinger");
    pid_t c=start(3,"/system/bin/a6l_input_client",0,"client");int status=0;
    int dump_done=0;
    for(;;){
        pid_t w=waitpid(c,&status,WNOHANG);need(w>=0,"client wait");if(w==c)break;
        if(!dump_done&&access(ROOT "/logs/dump-request",F_OK)==0){
            pid_t helper=start(8,"/system/bin/a6l_input_client",0,"dump-helper");int ds=0;
            need(waitpid(helper,&ds,0)==helper,"dump helper wait");children[8]=0;
            need(WIFEXITED(ds)&&WEXITSTATUS(ds)==0,"root dump helper result");
            int done=open(ROOT "/logs/dump-done",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
            need(done>=0,"dump completion marker");close(done);dump_done=1;
        }
        usleep(100000);
    }
    children[3]=0;printf("A6L_GRAPHICS_CLIENT_STATUS raw=%d\n",status);
    for(int i=0;i<3;i++){int s=0;pid_t p=waitpid(children[i],&s,WNOHANG);printf("A6L_GRAPHICS_SERVICE_STATUS slot=%d wait=%d raw=%d\n",i,p,s);need(p==0,"service remained alive");}
    {int s=0;need(waitpid(children[6],&s,WNOHANG)==0,"SurfaceFlinger remained alive");}
    {int s=0;need(waitpid(children[7],&s,WNOHANG)==0,"hardware service manager remained alive");}
    need(WIFEXITED(status)&&WEXITSTATUS(status)==0,"graphics client result");
    cleanup();
    need(!umount(ROOT "/dev/binderfs"),"binder cleanup");
    need(!umount(ROOT "/dev"),"devices cleanup");
    need(!umount(ROOT "/sys/fs/selinux"),"SELinux cleanup");
    need(!umount(ROOT "/sys"),"sys cleanup");need(!umount(ROOT "/proc"),"proc cleanup");
    puts("A6L_INPUT_SERVICES_PASS client=1 services_alive=5 private_properties=1 namespace_cleanup=1");return 0;
}
