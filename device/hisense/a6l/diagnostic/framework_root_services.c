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
#include <sys/resource.h>
#include <unistd.h>
#define ROOT "/"
static pid_t children[9];

static void cleanup(void) {
    for(int i=8;i>=0;i--)if(children[i]>0){kill(-children[i],SIGKILL);waitpid(children[i],NULL,0);children[i]=0;}
}
static void timedout(int sig){(void)sig;cleanup();_exit(124);}
static void need(int ok,const char *why){if(!ok){fprintf(stderr,"A6L_GRAPHICS_FAIL %s errno=%d %s\n",why,errno,strerror(errno));exit(1);}}
static void dir(const char *p){need(!mkdir(p,0755)||errno==EEXIST,p);struct stat s;need(!lstat(p,&s)&&S_ISDIR(s.st_mode),p);}
static void bind_ro(const char *src,const char *dst){dir(dst);need(!mount(src,dst,NULL,MS_BIND,NULL),dst);need(!mount(NULL,dst,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY,NULL),"readonly bind");}

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
#include "framework_root_properties.h"
static pid_t start(int slot,const char *bin,int vendor,const char *logname) {
    char log[256];snprintf(log,sizeof(log),ROOT "/logs/%s.log",logname);
    int fd=open(log,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);need(fd>=0,"fresh child log");
    pid_t p=fork();need(p>=0,"fork child");
    if(!p){setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(125);dup2(fd,1);dup2(fd,2);close(fd);
        if(chdir("/"))_exit(126);
        /* Credential changes clear PDEATHSIG; restore it for the system client. */
        if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()==1)_exit(125);
        setenv("LD_LIBRARY_PATH",vendor?"/vendor/lib64/hw:/vendor/lib64:/system/lib64":"/apex/com.android.art/lib64:/apex/com.android.runtime/lib64/bionic:/apex/com.android.i18n/lib64:/apex/com.android.conscrypt/lib64:/system/lib64:/vendor/lib64/hw:/vendor/lib64",1);
        if(slot==8)execl(bin,bin,"--dump-only",NULL);
        else if(slot==0&&access("/system/bin/strace",X_OK)==0)
            execl("/system/bin/strace","strace","-f","-o","/logs/servicemanager.strace",bin,NULL);
        else execl(bin,bin,NULL);
        fprintf(stderr,"exec %s failed: %s\n",bin,strerror(errno));_exit(127);
    }
    children[slot]=p;setpgid(p,p);close(fd);return p;
}

/* Real cgroup filesystems in the disposable VM; no synthetic scheduling files. */
static void setup_vm_cgroups(void) {
    const char *controllers[]={"cpu","blkio"};
    const char *paths[]={"/dev/cpuctl","/dev/blkio"};
    const char *groups[]={"", "foreground", "foreground_window", "background",
                         "top-app", "rt", "system", "system-background", "dex2oat"};
    for(int i=0;i<2;i++){
        dir(paths[i]);need(!mount("cgroup",paths[i],"cgroup",MS_NOSUID|MS_NODEV|MS_NOEXEC,controllers[i]),"VM cgroup v1");
        for(unsigned j=0;j<sizeof(groups)/sizeof(groups[0]);j++){
            char path[256],file[320];snprintf(path,sizeof(path),"%s/%s",paths[i],groups[j]);dir(path);
            need(!chown(path,1000,1000)&&!chmod(path,0775),"cgroup ownership");
            const char *files[]={"tasks","cgroup.procs"};
            for(int k=0;k<2;k++){
                snprintf(file,sizeof(file),"%s/%s",path,files[k]);
                need(!chown(file,1000,1000)&&!chmod(file,0664),"cgroup task permissions");
            }
        }
    }
    need(!mount("cgroup2","/sys/fs/cgroup","cgroup2",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL),"VM cgroup v2");
    const char *v2[]={"/sys/fs/cgroup","/sys/fs/cgroup/system","/sys/fs/cgroup/apps"};
    for(int i=0;i<3;i++){
        dir(v2[i]);need(!chown(v2[i],1000,1000)&&!chmod(v2[i],0775),"cgroup v2 ownership");
        char f[256];snprintf(f,sizeof(f),"%s/cgroup.subtree_control",v2[i]);
        int fd=open(f,O_WRONLY|O_CLOEXEC);need(fd>=0,"memory controller file");
        need(write(fd,"+memory",7)==7,"memory controller activation");close(fd);
    }
    puts("A6L_VM_CGROUPS_READY cpu_v1=1 blkio_v1=1 memory_v2=1 cpuset_v1=0");
}
/* Where may the framework supervisor run?  (1) the diskless QEMU "virt" machine, or (2) V71+: the spare phone,
 * but only on the reviewed V68 diagnostic image AND after the attended host runner created the approval file.
 * Returns 1 for QEMU, 2 for an approved phone run, 0 otherwise. */
static int a6l_environment(void) {
    char model[128]={0},mark[16]={0};int f=open("/proc/device-tree/model",O_RDONLY);
    if(f>=0){(void)!read(f,model,127);close(f);}
    if(strstr(model,"virt"))return 1;
    f=open("/proc/device-tree/chosen/hisense,a6l-controls",O_RDONLY);if(f>=0){(void)!read(f,mark,15);close(f);}
    return !strcmp(mark,"v68")&&!access("/tmp/a6l-framework-phone-approved",F_OK)?2:0;
}
static void launch_zygote(void) {
    need(a6l_environment()!=0&&getuid()==0,"QEMU or approved-phone root zygote only");
    /* Match init's descriptor hygiene; never leak supervisor/shell log FDs. */
    int nullfd=open("/dev/null",O_RDWR);need(nullfd>=0,"zygote null stdio");
    need(dup2(nullfd,0)>=0&&dup2(nullfd,1)>=0&&dup2(nullfd,2)>=0,"zygote stdio");
    if(nullfd>2)close(nullfd);
    need(!syscall(SYS_close_range,3u,~0u,0),"zygote inherited fd cleanup");
    const char *names[]={"zygote","usap_pool_primary"};
    for(int i=0;i<2;i++){
        int fd=socket(AF_UNIX,SOCK_STREAM,0);need(fd>=0,"zygote socket");
        struct sockaddr_un a={.sun_family=AF_UNIX};
        snprintf(a.sun_path,sizeof(a.sun_path),"/dev/socket/%s",names[i]);
        need(!bind(fd,(struct sockaddr *)&a,sizeof(a)),"zygote socket bind");
        need(!chmod(a.sun_path,0660)&&!chown(a.sun_path,0,1000),"zygote socket permissions");
        char var[80],value[32];snprintf(var,sizeof(var),"ANDROID_SOCKET_%s",names[i]);
        snprintf(value,sizeof(value),"%d",fd);need(!setenv(var,value,1),"zygote socket export");
    }
    struct rlimit nice={40,40};need(!setrlimit(RLIMIT_NICE,&nice),"zygote nice limit");
    execl("/system/bin/app_process64","app_process64","-Xzygote","/system/bin",
          "--zygote","--start-system-server","--socket-name=zygote",NULL);
    need(0,"zygote exec");
}
static void setup_vm_apex_nodes(void) {
    if(access("/system/bin/framework-apex-v51.sh",X_OK))return;
    dir("/dev/block");
    FILE *f=fopen("/proc/misc","r");need(f!=NULL,"VM misc devices");
    char line[160],name[80],path[128];unsigned number;int have_loop=0,have_dm=0;
    while(fgets(line,sizeof(line),f)) {
      if(sscanf(line,"%u %79s",&number,name)==2){
        if(!strcmp(name,"loop-control")||!strcmp(name,"device-mapper")){
            snprintf(path,sizeof(path),"/dev/%s",name);
            need(!mknod(path,S_IFCHR|0600,makedev(10,number)),"VM APEX control node");
            if(!strcmp(name,"loop-control"))have_loop=1;else have_dm=1;
        }
      }
    }
    fclose(f);need(have_loop&&have_dm,"APEX control drivers present");
    unsigned dm_major=0;
    f=fopen("/proc/devices","r");need(f!=NULL,"VM device majors");
    while(fgets(line,sizeof(line),f))if(sscanf(line,"%u %79s",&number,name)==2&&!strcmp(name,"device-mapper"))dm_major=number;
    fclose(f);need(dm_major>0,"VM mapper block major");
    for(unsigned i=0;i<128;i++){
        snprintf(path,sizeof(path),"/dev/block/loop%u",i);
        need(!mknod(path,S_IFBLK|0600,makedev(7,i)),"VM loop node");
        snprintf(path,sizeof(path),"/dev/block/dm-%u",i);
        need(!mknod(path,S_IFBLK|0600,makedev(dm_major,i)),"VM mapper node");
    }
    puts("A6L_VM_APEX_NODES_READY diskless=1");
}
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--zygote")){launch_zygote();return 1;}
    need(argc==1,"framework arguments");
    umask(022); /* Android init may start this supervisor with umask 077. */
    setbuf(stdout,NULL);atexit(cleanup);signal(SIGALRM,timedout);signal(SIGTERM,timedout);alarm(1800); /* V62: whole-VM budget; 300 s no longer covers the staged daemons + SystemServer under TCG */
    need(a6l_environment()!=0,"QEMU or approved V68 phone run only");printf("A6L_ENVIRONMENT=%s\n",a6l_environment()==1?"qemu":"phone-approved");
    char prop[PROP_VALUE_MAX];__system_property_get("ro.a6l.ramdiag",prop);need(getuid()==0&&!strcmp(prop,"v38"),"V38 root only");
    struct statfs fs;need(!statfs(ROOT,&fs)&&(unsigned long)fs.f_type==0x01021994,"RAM root required");
    need(!unshare(CLONE_NEWNS),"private mount namespace");need(!mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL),"private propagation");
    dir(ROOT "/proc");need(!mount("proc",ROOT "/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL),"private proc");
    dir("/v49-oldselinux");
    need(!mount("/sys/fs/selinux","/v49-oldselinux",NULL,MS_BIND,NULL),"preserve SELinux mount");
    bind_ro("/sys",ROOT "/sys");
    need(!mount("/v49-oldselinux",ROOT "/sys/fs/selinux",NULL,MS_BIND,NULL),"SELinux status bind");
    bind_ro("/dev","/v49-olddev");
    dir(ROOT "/dev");need(!mount("tmpfs",ROOT "/dev","tmpfs",MS_NOSUID,"size=16m,mode=0755"),"private devices");
    need(!mknod(ROOT "/dev/null",S_IFCHR|0666,makedev(1,3)),"null node");
    need(!mknod(ROOT "/dev/zero",S_IFCHR|0666,makedev(1,5)),"zero node");
    need(!mknod(ROOT "/dev/random",S_IFCHR|0666,makedev(1,8)),"random node");
    need(!mknod(ROOT "/dev/urandom",S_IFCHR|0666,makedev(1,9)),"urandom node");
    need(!chmod(ROOT "/dev/null",0666)&&!chmod(ROOT "/dev/zero",0666)&&
         !chmod(ROOT "/dev/random",0666)&&!chmod(ROOT "/dev/urandom",0666),"standard device permissions");
    need(!mknod(ROOT "/dev/kmsg",S_IFCHR|0600,makedev(1,11)),"debug log node");
    setup_vm_apex_nodes();
    dir(ROOT "/dev/dri");need(!mknod(ROOT "/dev/dri/card0",S_IFCHR|0600,makedev(226,0)),"DRM node");
    need(!chown(ROOT "/dev/dri/card0",1000,1000),"system DRM ownership");
    /* V71: expose existing evdev nodes (touch, keys) inside the private /dev so EventHub can see them. Best effort. */
    dir(ROOT "/dev/input");
    for(unsigned i=0;i<16;i++){char sp[64],dp[64],v[32]={0};unsigned ma,mi;snprintf(sp,sizeof(sp),"/sys/class/input/event%u/dev",i);
        int sf=open(sp,O_RDONLY|O_CLOEXEC);if(sf<0)continue;ssize_t n=read(sf,v,sizeof(v)-1);close(sf);
        if(n<=0||sscanf(v,"%u:%u",&ma,&mi)!=2)continue;snprintf(dp,sizeof(dp),ROOT "/dev/input/event%u",i);
        if(!mknod(dp,S_IFCHR|0660,makedev(ma,mi))){(void)!chown(dp,0,1004);printf("A6L_INPUT_NODE event%u %u:%u\n",i,ma,mi);}
    }
    dir(ROOT "/dev/binderfs");need(!mount("binder",ROOT "/dev/binderfs","binder",MS_NOSUID|MS_NOEXEC,NULL),"private binderfs");
    int fd=open(ROOT "/dev/binderfs/binder-control",O_RDONLY|O_CLOEXEC);need(fd>=0,"binder control");
    struct binderfs_device b={0};strcpy(b.name,"a6l-v48");need(!ioctl(fd,BINDER_CTL_ADD,&b),"private binder add");strcpy(b.name,"a6l-v48-hw");need(!ioctl(fd,BINDER_CTL_ADD,&b),"private hwbinder add");close(fd);
    need(!symlink("binderfs/a6l-v48",ROOT "/dev/binder"),"binder alias");
    need(!symlink("binderfs/a6l-v48-hw",ROOT "/dev/hwbinder"),"hwbinder alias");
    need(!chown(ROOT "/dev/binderfs/a6l-v48",1000,1000),"system Binder ownership");
    need(!chown(ROOT "/dev/binderfs/a6l-v48-hw",1000,1000),"system HWBinder ownership");
    need(!chmod("/dev/binderfs/a6l-v48",0666)&&!chmod("/dev/binderfs/a6l-v48-hw",0666),"standard Binder device permissions");
    dir(ROOT "/logs");
    need(!chown(ROOT "/logs",1000,1000),"system log directory");
    setup_vm_cgroups();
    start_logger();
    children[5]=start_private_properties();
    puts("A6L_GRAPHICS_NAMESPACE_READY private_binder=1 persistent_mounts=0");
    start(0,"/system/bin/servicemanager",0,"servicemanager");usleep(300000);
    start(7,"/system/bin/hwservicemanager",0,"hwservicemanager");
    start(1,"/vendor/bin/hw/android.hardware.graphics.allocator-service.minigbm",1,"allocator");
    start(2,"/vendor/bin/hw/android.hardware.composer.hwc3-service.drm",1,"composer");
    start(6,"/system/bin/surfaceflinger",0,"surfaceflinger");
    pid_t c=start(3,"/system/bin/framework-probe.sh",0,"framework");int status=0;
    need(waitpid(c,&status,0)==c,"framework wait");
    /* Keep the process-group ID: background native daemons outlive the shell. */
    printf("A6L_FRAMEWORK_STATUS raw=%d\n",status);
    need(WIFEXITED(status)&&WEXITSTATUS(status)==0,"runtime probes result");
    cleanup();
    if(!access("/system/bin/framework-bpf-v55.sh",X_OK))
        need(!umount("/sys/fs/bpf"),"BPF cleanup");
    need(!umount("/dev/cpuctl")&&!umount("/dev/blkio")&&!umount("/sys/fs/cgroup"),"cgroups cleanup");
    need(!umount(ROOT "/dev/binderfs"),"binder cleanup");
    need(!umount(ROOT "/dev"),"devices cleanup");
    need(!umount(ROOT "/sys/fs/selinux"),"SELinux cleanup");
    need(!umount("/v49-oldselinux"),"old selinux cleanup");
    need(!umount(ROOT "/sys"),"sys cleanup");need(!umount(ROOT "/proc"),"proc cleanup");
    need(!umount("/v49-olddev"),"old devices cleanup");
    puts("A6L_FRAMEWORK_SERVICES_PASS runtime=1 namespace_cleanup=1");return 0;
}
