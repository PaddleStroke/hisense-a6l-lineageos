/* RAM test property service. Uses fresh Bionic property areas inside the chroot. */
#include <stdint.h>
#include <sys/time.h>
extern int __system_property_area_init(void);
extern int __system_property_add(const char *, unsigned int, const char *, unsigned int);
extern int __system_property_update(prop_info *, const char *, unsigned int);
struct saved_property { char *name, *value; };
static struct saved_property saved[2048];
static size_t saved_count;
static void save_value(void *unused,const char *name,const char *value,uint32_t serial) {
    (void)unused;(void)serial;need(saved_count<2048,"property snapshot bound");
    saved[saved_count++]=(struct saved_property){strdup(name),strdup(value)};
}
static void snapshot_property(const prop_info *pi,void *unused) {
    __system_property_read_callback(pi,save_value,unused);
}
static void prop_write(const char *name,const char *value) {
    prop_info *pi=(prop_info *)__system_property_find(name);
    int result=pi?__system_property_update(pi,value,strlen(value)):
                  __system_property_add(name,strlen(name),value,strlen(value));
    need(result==0,name);
}
static int read_full(int fd,void *p,size_t count) {
    while(count){ssize_t n=read(fd,p,count);if(n<=0)return -1;p=(char *)p+n;count-=(size_t)n;}return 0;
}
static int read_string(int fd,char *s,uint32_t max) {
    uint32_t len;if(read_full(fd,&len,sizeof(len))||len>=max)return -1;
    if(read_full(fd,s,len))return -1;s[len]=0;return 0;
}
static pid_t start_private_properties(void) {
    need(!__system_property_foreach(snapshot_property,NULL),"snapshot diagnostic properties");
    dir(ROOT "/dev/__properties__");
    int in=open("/dev/__properties__/property_info",O_RDONLY|O_CLOEXEC);
    int out=open(ROOT "/dev/__properties__/property_info",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0444);
    need(in>=0&&out>=0,"private property schema");char buf[8192];ssize_t n;
    while((n=read(in,buf,sizeof(buf)))>0)need(write(out,buf,n)==n,"copy property schema");
    need(n==0,"read property schema");close(in);close(out);
    int ready[2];need(!pipe2(ready,O_CLOEXEC),"property readiness pipe");
    pid_t p=fork();need(p>=0,"private property fork");
    if(!p){
        close(ready[0]);setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(125);
        need(!chroot(ROOT)&&!chdir("/"),"property chroot");
        need(!__system_property_area_init(),"fresh property areas");
        for(size_t i=0;i<saved_count;i++)prop_write(saved[i].name,saved[i].value);
        prop_write("servicemanager.ready","false");
        prop_write("ro.hardware.vulkan","pastel");
        prop_write("ro.surface_flinger.default_composition_pixel_format","5");
        prop_write("debug.renderengine.backend","skiaglthreaded");
        prop_write("ro.hardware.egl","angle");
        prop_write("hwservicemanager.disabled","false");
        prop_write("ro.sf.lcd_density","400");
        prop_write("debug.sf.nobootanimation","1");
        prop_write("service.sf.prime_shader_cache","false");
        FILE *runtime=fopen("/system/etc/a6l-runtime.prop","r");need(runtime!=NULL,"runtime properties");
        char line[512];while(fgets(line,sizeof(line),runtime)){
            line[strcspn(line,"\r\n")]=0;char *eq=strchr(line,'=');
            if(eq&&line[0]!='#'){*eq++=0;prop_write(line,eq);}
        }fclose(runtime);
        int s=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);need(s>=0,"property socket");
        struct sockaddr_un a={.sun_family=AF_UNIX};strcpy(a.sun_path,"/dev/socket/property_service");
        need(!bind(s,(struct sockaddr *)&a,sizeof(a))&&!listen(s,8),"property listen");need(!chmod(a.sun_path,0666),"private property socket mode");
        need(write(ready[1],"R",1)==1,"property ready");close(ready[1]);
        for(;;){
            int c=accept4(s,NULL,NULL,SOCK_CLOEXEC);if(c<0)continue;
            struct timeval timeout={.tv_sec=2};setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
            uint32_t cmd=0,result=0x18;char name[256],value[PROP_VALUE_MAX];
            if(!read_full(c,&cmd,4)&&cmd==0x00020001&&!read_string(c,name,sizeof(name))&&!read_string(c,value,sizeof(value))){
                if(strcmp(name,"service.sf.present_timestamp")==0||strcmp(name,"servicemanager.ready")==0||strcmp(name,"hwservicemanager.ready")==0||strcmp(name,"hwservicemanager.disabled")==0||strncmp(name,"debug.",6)==0||strncmp(name,"sys.system_server.",18)==0||strncmp(name,"persist.sys.",12)==0||strcmp(name,"sys.boot_completed")==0){prop_write(name,value);result=0;}
                fprintf(stdout,"A6L_PRIVATE_PROPERTY name=%s result=%u\n",name,result);
                (void)write(c,&result,4);
            }
            close(c);
        }
    }
    setpgid(p,p);close(ready[1]);char mark=0;need(read(ready[0],&mark,1)==1&&mark=='R',"private properties ready");close(ready[0]);
    for(size_t i=0;i<saved_count;i++){free(saved[i].name);free(saved[i].value);}return p;
}
