"""Prepare isolated framework runtime sources. Never access the phone."""
from pathlib import Path
R=Path(__file__).resolve().parents[1]
D=R/'device/hisense/a6l/diagnostic'
s=(D/'input_services.c').read_text()
s=s.replace('a6l-v47','a6l-v48').replace('private_properties.h','framework_properties.h')
a=s.index('/* Expose exactly one'); b=s.index('static void start_logger',a)
s=s[:a]+s[b:]
s=s.replace('    expose_touch();\n','')
a=s.index('        if(slot==3){'); b=s.index('        /* Credential',a)
s=s[:a]+s[b:]
s=s.replace('alarm(240)','alarm(300)')
s=s.replace('#include <sys/wait.h>','#include <sys/wait.h>\n#include <sys/resource.h>')
s=s.replace('int main(void) {','''static void launch_zygote(void) {
    char model[128]={0};int mf=open("/proc/device-tree/model",O_RDONLY);
    need(mf>=0&&read(mf,model,127)>0,"zygote QEMU model");close(mf);
    need(strstr(model,"virt")!=NULL&&getuid()==0,"QEMU root zygote only");
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
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--zygote")){launch_zygote();return 1;}
    need(argc==1,"framework arguments");''')
s=s.replace('"/system/lib64:/vendor/lib64/hw:/vendor/lib64"','"/apex/com.android.art/lib64:/apex/com.android.runtime/lib64/bionic:/apex/com.android.i18n/lib64:/apex/com.android.conscrypt/lib64:/system/lib64:/vendor/lib64/hw:/vendor/lib64"')
a=s.index('    pid_t c=start(3,'); b=s.index('    cleanup();',a)
s=s[:a]+'''    pid_t c=start(3,"/system/bin/framework-probe.sh",0,"framework");int status=0;
    need(waitpid(c,&status,0)==c,"framework wait");children[3]=0;
    printf("A6L_FRAMEWORK_STATUS raw=%d\\n",status);
    need(WIFEXITED(status)&&WEXITSTATUS(status)==0,"runtime probes result");
'''+s[b:]
s=s.replace('A6L_INPUT_SERVICES_PASS client=1 services_alive=5 private_properties=1 namespace_cleanup=1','A6L_FRAMEWORK_SERVICES_PASS runtime=1 namespace_cleanup=1')
# This harness is not approved for real hardware. The host test is diskless.
s=s.replace('    char prop[PROP_VALUE_MAX];','    char model[128]={0};int mf=open("/proc/device-tree/model",O_RDONLY);need(mf>=0,"QEMU model");need(read(mf,model,127)>0,"model read");close(mf);need(strstr(model,"virt")!=NULL,"V48 offline QEMU only");\n    char prop[PROP_VALUE_MAX];')
(D/'framework_services.c').write_text(s)
p=(D/'private_properties.h').read_text()
p=p.replace('need(!bind(s,(struct sockaddr *)&a,sizeof(a))&&!listen(s,8),"property listen");',
    'need(!bind(s,(struct sockaddr *)&a,sizeof(a))&&!listen(s,8),"property listen");need(!chmod(a.sun_path,0666),"private property socket mode");')
p=p.replace('strncmp(name,"debug.",6)==0',
    'strncmp(name,"debug.",6)==0||strncmp(name,"sys.system_server.",18)==0||strncmp(name,"persist.sys.",12)==0||strcmp(name,"sys.boot_completed")==0')
needle='        int s=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);'
setup='''        FILE *runtime=fopen("/system/etc/a6l-runtime.prop","r");need(runtime!=NULL,"runtime properties");
        char line[512];while(fgets(line,sizeof(line),runtime)){
            line[strcspn(line,"\\r\\n")]=0;char *eq=strchr(line,'=');
            if(eq&&line[0]!='#'){*eq++=0;prop_write(line,eq);}
        }fclose(runtime);
'''
p=p.replace(needle,setup+needle)
(D/'framework_properties.h').write_text(p)
bp=R/'device/hisense/a6l/Android.bp'
old=bp.read_text()
if 'name: "a6l_framework_services"' not in old:
    bp.write_text(old+'''\ncc_binary {
    name: "a6l_framework_services",
    srcs: ["diagnostic/framework_services.c"],
    static_executable: true,
    compile_multilib: "64",
    system_shared_libs: [],
    static_libs: ["libc"],
    stl: "none",
    cflags: ["-Wall", "-Wextra", "-Werror"],
}
''')
print('V48 offline supervisor prepared; phone execution rejected by model guard')
