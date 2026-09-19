/* V39 live RAM test. Only fixed system/vendor mounts, read-only, no replay.
 * Binder runs in a fresh private mount namespace and a private binderfs device.
 * No persistent file is opened for writing, and no persistent service is started.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binderfs.h>
#include <linux/fs.h>
#include <openssl/sha.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT "/tmp/a6l-v39"
static void die(const char *s) { fprintf(stderr, "A6L_INTEGRATION_FAIL %s errno=%d %s\n",s,errno,strerror(errno)); exit(1); }
static void require(int ok, const char *s) { if (!ok) die(s); }
static void directory(const char *p) { if (mkdir(p,0700) && errno != EEXIST) die(p); struct stat s; require(!lstat(p,&s)&&S_ISDIR(s.st_mode),p); }
static void read_text(const char *p, char *s, size_t cap) {
    int fd=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW); if(fd<0) die(p);
    ssize_t n=read(fd,s,cap-1); require(n>=0,p); s[n]=0; close(fd);
    s[strcspn(s,"\r\n")]=0;
}
static void expected_text(const char *p,const char *expected) {
    char s[512];read_text(p,s,sizeof(s));require(!strcmp(s,expected),p);
}
static uint64_t number(const char *p) {
    char s[128],*end;read_text(p,s,sizeof(s));errno=0;
    unsigned long long v=strtoull(s,&end,10);require(!errno&&end!=s&&!*end,p);return v;
}
static void file_hash(const char *p,const char *expected) {
    int fd=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)die(p);
    struct stat st;require(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size<1048576,"hash regular small file");
    SHA256_CTX c;SHA256_Init(&c);unsigned char b[8192],h[32];ssize_t n;
    while((n=read(fd,b,sizeof(b)))>0)SHA256_Update(&c,b,(size_t)n);
    require(n==0,"hash read");close(fd);SHA256_Final(h,&c);
    char hex[65];for(int i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",h[i]);
    printf("A6L_FS_HASH path=%s sha256=%s match=%d\n",p,hex,!strcmp(hex,expected));
    require(!strcmp(hex,expected),"filesystem hash");
}
struct part {const char *name;unsigned index;uint64_t start,bytes;};
static void filesystem(const struct part *part) {
    char sys[160],p[200],node[160],mnt[160],uevent[512];
    snprintf(sys,sizeof(sys),"/sys/block/mmcblk1/mmcblk1p%u",part->index);
    snprintf(p,sizeof(p),"%s/start",sys);require(number(p)==part->start,"partition start");
    snprintf(p,sizeof(p),"%s/size",sys);require(number(p)==part->bytes/512,"partition size");
    snprintf(p,sizeof(p),"%s/partition",sys);require(number(p)==part->index,"partition index");
    snprintf(p,sizeof(p),"%s/dev",sys);char dev[32];snprintf(dev,sizeof(dev),"179:%u",part->index);expected_text(p,dev);
    snprintf(p,sizeof(p),"%s/uevent",sys);
    int u=open(p,O_RDONLY|O_CLOEXEC);require(u>=0,"uevent open");ssize_t n=read(u,uevent,sizeof(uevent)-1);require(n>0,"uevent read");close(u);uevent[n]=0;
    char name[64];snprintf(name,sizeof(name),"PARTNAME=%s\n",part->name);require(strstr(uevent,name)!=NULL,"partition name");
    snprintf(node,sizeof(node),ROOT "/work/%s.block",part->name);
    require(!mknod(node,S_IFBLK|0400,makedev(179,part->index)),"fresh block node");
    int fd=open(node,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);require(fd>=0,"open fixed read-only block");
    uint64_t size=0;require(!ioctl(fd,BLKGETSIZE64,&size)&&size==part->bytes,"block capacity");
    int ro=1;require(!ioctl(fd,BLKROSET,&ro),"set block read-only");ro=0;require(!ioctl(fd,BLKROGET,&ro)&&ro==1,"confirm block read-only");
    unsigned char magic[2];require(pread(fd,magic,2,1024+56)==2&&magic[0]==0x53&&magic[1]==0xef,"ext4 magic");close(fd);
    snprintf(mnt,sizeof(mnt),ROOT "/work/%s",part->name);directory(mnt);
    require(!mount(node,mnt,"ext4",MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC,"noload"),"ext4 ro,noload mount");
    struct statfs fs;require(!statfs(mnt,&fs)&&(unsigned long)fs.f_type==0xef53&&(fs.f_flags&ST_RDONLY),"readonly ext4 state");
    printf("A6L_FS_MOUNT name=%s ro=1 noload=1 block_ro=1 bytes=%llu\n",part->name,(unsigned long long)size);
    if(part->index==43) {
        snprintf(p,sizeof(p),"%s/system/build.prop",mnt);file_hash(p,"bdcdf3e085bc1d40fb7a59236a28a89ba90ba63aa043a4299547cb974954e8c5");
    } else {
        snprintf(p,sizeof(p),"%s/build.prop",mnt);file_hash(p,"8ea2baeeb0634e7402280072ac1cf59411d4a23068e2a2ff459592ccb11c4397");
        snprintf(p,sizeof(p),"%s/etc/fstab.qcom",mnt);file_hash(p,"dc87e2e57f5e697a9a785a79937d0442d910e9adf5753721f8769e799402164a");
        snprintf(p,sizeof(p),"%s/etc/vintf/manifest.xml",mnt);file_hash(p,"6ad354b74b5497d875b8ddfa839e0c82aad41e642b3a3650de20e7c1c77de4a3");
        snprintf(p,sizeof(p),"%s/etc/selinux/plat_sepolicy_vers.txt",mnt);file_hash(p,"1251c6c76b6c0df5dc99c090ee281e2f03c3526b43314c548e789cf5ca81f577");
    }
    require(!umount(mnt),"unmount fixed filesystem");
    printf("A6L_FS_UNMOUNT name=%s success=1\n",part->name);
}
static void binder(void) {
    directory(ROOT "/work/binder");
    require(!mount("binder",ROOT "/work/binder","binder",MS_NOSUID|MS_NOEXEC,NULL),"private binderfs");
    int fd=open(ROOT "/work/binder/binder-control",O_RDONLY|O_CLOEXEC);require(fd>=0,"binder control");
    struct binderfs_device d={0};strcpy(d.name,"a6l-test");require(!ioctl(fd,BINDER_CTL_ADD,&d),"binder device add");close(fd);
    printf("A6L_BINDER_DEVICE major=%u minor=%u\n",d.major,d.minor);
    pid_t server=fork();require(server>=0,"fork servicemanager");
    if(!server) {prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(126);setenv("LD_LIBRARY_PATH",ROOT "/lib64",1);execl(ROOT "/bin/servicemanager","servicemanager",ROOT "/work/binder/a6l-test",NULL);_exit(127);}
    sleep(1);
    pid_t client=fork();require(client>=0,"fork binder client");
    if(!client) {prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(126);setenv("LD_LIBRARY_PATH",ROOT "/lib64",1);execl(ROOT "/bin/a6l_binder_client","a6l_binder_client",ROOT "/work/binder/a6l-test",NULL);_exit(127);}
    int status=0;require(waitpid(client,&status,0)==client,"wait binder client");
    int probe=0;pid_t state=waitpid(server,&probe,WNOHANG);
    if(state==0){kill(server,SIGTERM);waitpid(server,&probe,0);}
    require(state==0&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"binder transaction");
    require(!umount(ROOT "/work/binder"),"binder unmount");
    puts("A6L_BINDER_PASS checkService=1 pingBinder=1 private_namespace=1");
}
int main(int argc,char **argv) {
    setbuf(stdout,NULL);alarm(35);
    require(argc==2&&(!strcmp(argv[1],"--filesystems")||!strcmp(argv[1],"--binder")),"fixed mode required");
    require(getuid()==0&&getpid()!=1,"root non-init process");
    char prop[PROP_VALUE_MAX];__system_property_get("ro.a6l.ramdiag",prop);require(!strcmp(prop,"v38"),"V38 environment required");
    char init[128];ssize_t n=readlink("/proc/1/exe",init,sizeof(init)-1);require(n>0,"PID1 executable");init[n]=0;require(!strcmp(init,"/system/bin/init"),"Android init");
    struct statfs fs;require(!statfs("/tmp",&fs)&&(unsigned long)fs.f_type==0x01021994,"tmpfs staging required");
    require(!unshare(CLONE_NEWNS),"private mount namespace");require(!mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL),"private propagation");
    directory(ROOT "/work");require(!mount("tmpfs",ROOT "/work","tmpfs",MS_NOSUID,"size=1m,mode=0700"),"private work tmpfs");
    if(!strcmp(argv[1],"--binder"))binder();
    else {
        expected_text("/sys/block/mmcblk1/device/name","hDEaP3");
        expected_text("/sys/block/mmcblk1/device/manfid","0x000090");
        require(number("/sys/block/mmcblk1/size")==244285440,"eMMC capacity");
        const struct part parts[]={{"system",43,1589248,6442450944ULL},{"vendor",44,14172160,1153433600ULL}};
        for(unsigned i=0;i<2;i++)filesystem(&parts[i]);
        puts("A6L_FILESYSTEMS_PASS partitions=2 hashes=5 persistent_writes=0");
    }
    require(!umount(ROOT "/work"),"private work cleanup");
    puts("A6L_INTEGRATION_DONE");return 0;
}
