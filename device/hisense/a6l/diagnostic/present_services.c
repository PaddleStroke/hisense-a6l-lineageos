/* Isolated RAM-only Android graphics service environment. No block devices. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binderfs.h>
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
#include <sys/wait.h>
#include <unistd.h>
#define ROOT "/tmp/a6l-v43/root"
static pid_t children[5];
static char old_ready[PROP_VALUE_MAX];
static int restore_ready,restore_failed;
static void cleanup(void) {
    for(int i=4;i>=0;i--)if(children[i]>0){kill(-children[i],SIGKILL);waitpid(children[i],NULL,0);children[i]=0;}
    if(restore_ready){restore_failed=__system_property_set("servicemanager.ready",old_ready)!=0;if(!restore_failed)restore_ready=0;}
}
static void timedout(int sig){(void)sig;cleanup();_exit(124);}
static void need(int ok,const char *why){if(!ok){fprintf(stderr,"A6L_GRAPHICS_FAIL %s errno=%d %s\n",why,errno,strerror(errno));exit(1);}}
static void dir(const char *p){need(!mkdir(p,0755)||errno==EEXIST,p);struct stat s;need(!lstat(p,&s)&&S_ISDIR(s.st_mode),p);}
static void bind_ro(const char *src,const char *dst){dir(dst);need(!mount(src,dst,NULL,MS_BIND,NULL),dst);need(!mount(NULL,dst,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY,NULL),"readonly bind");}
static void start_logger(void) {
    dir(ROOT "/dev/socket");int s=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);need(s>=0,"log socket");
    struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,ROOT "/dev/socket/logdw");
    need(!bind(s,(struct sockaddr*)&addr,sizeof(addr)),"log socket bind");
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
static void bind_property_socket(const char *name) {
    char src[128],dst[256];snprintf(src,sizeof(src),"/dev/socket/%s",name);snprintf(dst,sizeof(dst),ROOT "/dev/socket/%s",name);
    struct stat st;need(!lstat(src,&st)&&S_ISSOCK(st.st_mode),"original property socket");
    int fd=open(dst,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);need(fd>=0,"property socket mountpoint");close(fd);
    need(!mount(src,dst,NULL,MS_BIND,NULL),"bind property socket");
}
static pid_t start(int slot,const char *bin,int vendor,const char *logname) {
    char log[256];snprintf(log,sizeof(log),ROOT "/logs/%s.log",logname);
    int fd=open(log,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);need(fd>=0,"fresh child log");
    pid_t p=fork();need(p>=0,"fork child");
    if(!p){setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(125);dup2(fd,1);dup2(fd,2);close(fd);
        if(chroot(ROOT)||chdir("/"))_exit(126);
        setenv("LD_LIBRARY_PATH",vendor?"/vendor/lib64/hw:/vendor/lib64:/system/lib64":"/system/lib64",1);
        if(slot==0&&access("/system/bin/strace",X_OK)==0)
            execl("/system/bin/strace","strace","-f","-o","/logs/servicemanager.strace",bin,NULL);
        else execl(bin,bin,NULL);
        fprintf(stderr,"exec %s failed: %s\n",bin,strerror(errno));_exit(127);
    }
    children[slot]=p;setpgid(p,p);close(fd);return p;
}
int main(void) {
    setbuf(stdout,NULL);atexit(cleanup);signal(SIGALRM,timedout);signal(SIGTERM,timedout);alarm(65);
    char prop[PROP_VALUE_MAX];__system_property_get("ro.a6l.ramdiag",prop);need(getuid()==0&&!strcmp(prop,"v38"),"V38 root only");
    struct statfs fs;need(!statfs(ROOT,&fs)&&(unsigned long)fs.f_type==0x01021994,"RAM root required");
    need(!unshare(CLONE_NEWNS),"private mount namespace");need(!mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL),"private propagation");
    dir(ROOT "/proc");need(!mount("proc",ROOT "/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL),"private proc");
    bind_ro("/sys",ROOT "/sys");
    need(!mount("/sys/fs/selinux",ROOT "/sys/fs/selinux",NULL,MS_BIND,NULL),"SELinux status bind");
    dir(ROOT "/dev");need(!mount("tmpfs",ROOT "/dev","tmpfs",MS_NOSUID,"size=4m,mode=0755"),"private devices");
    bind_ro("/dev/__properties__",ROOT "/dev/__properties__");
    need(!mknod(ROOT "/dev/null",S_IFCHR|0666,makedev(1,3)),"null node");
    need(!mknod(ROOT "/dev/zero",S_IFCHR|0666,makedev(1,5)),"zero node");
    need(!mknod(ROOT "/dev/random",S_IFCHR|0666,makedev(1,8)),"random node");
    need(!mknod(ROOT "/dev/urandom",S_IFCHR|0666,makedev(1,9)),"urandom node");
    need(!mknod(ROOT "/dev/kmsg",S_IFCHR|0600,makedev(1,11)),"debug log node");
    dir(ROOT "/dev/dri");need(!mknod(ROOT "/dev/dri/card0",S_IFCHR|0600,makedev(226,0)),"DRM node");
    dir(ROOT "/dev/binderfs");need(!mount("binder",ROOT "/dev/binderfs","binder",MS_NOSUID|MS_NOEXEC,NULL),"private binderfs");
    int fd=open(ROOT "/dev/binderfs/binder-control",O_RDONLY|O_CLOEXEC);need(fd>=0,"binder control");
    struct binderfs_device b={0};strcpy(b.name,"a6l-v43");need(!ioctl(fd,BINDER_CTL_ADD,&b),"private binder add");close(fd);
    need(!symlink("binderfs/a6l-v43",ROOT "/dev/binder"),"binder alias");
    dir(ROOT "/logs");
    start_logger();
    __system_property_get("servicemanager.ready",old_ready);
    bind_property_socket("property_service");bind_property_socket("property_service_for_system");restore_ready=1;
    puts("A6L_GRAPHICS_NAMESPACE_READY private_binder=1 persistent_mounts=0");
    start(0,"/system/bin/servicemanager",0,"servicemanager");usleep(300000);
    start(1,"/vendor/bin/hw/android.hardware.graphics.allocator-service.minigbm",1,"allocator");
    start(2,"/vendor/bin/hw/android.hardware.composer.hwc3-service.drm",1,"composer");
    pid_t c=start(3,"/vendor/bin/a6l_graphics_client",1,"client");int status=0;
    need(waitpid(c,&status,0)==c,"client wait");children[3]=0;printf("A6L_GRAPHICS_CLIENT_STATUS raw=%d\n",status);
    for(int i=0;i<3;i++){int s=0;pid_t p=waitpid(children[i],&s,WNOHANG);printf("A6L_GRAPHICS_SERVICE_STATUS slot=%d wait=%d raw=%d\n",i,p,s);need(p==0,"service remained alive");}
    need(WIFEXITED(status)&&WEXITSTATUS(status)==0,"graphics client result");
    cleanup();
    need(!restore_failed,"restore readiness property");
    need(!umount(ROOT "/dev/socket/property_service"),"property socket cleanup");
    need(!umount(ROOT "/dev/socket/property_service_for_system"),"system property socket cleanup");
    need(!umount(ROOT "/dev/__properties__"),"properties cleanup");
    need(!umount(ROOT "/dev/binderfs"),"binder cleanup");
    need(!umount(ROOT "/dev"),"devices cleanup");
    need(!umount(ROOT "/sys/fs/selinux"),"SELinux cleanup");
    need(!umount(ROOT "/sys"),"sys cleanup");need(!umount(ROOT "/proc"),"proc cleanup");
    puts("A6L_GRAPHICS_SERVICES_PASS client=1 services_alive=3 namespace_cleanup=1");return 0;
}
