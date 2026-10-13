"""Create private graphics/input supervisor from the physically validated V44 scaffold."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
diag=ROOT/'device/hisense/a6l/diagnostic'
out=diag/'input_services.c';assert not out.exists()
s=(diag/'surface_services.c').read_text().replace('a6l-v44','a6l-v47')
s=s.replace('#include <errno.h>','#include <errno.h>\n#include <dirent.h>\n#include <linux/input.h>')
s=s.replace('alarm(100)','alarm(240)')
helper=r'''
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
        f=open(path,O_RDONLY|O_CLOEXEC);need(f>=0,"open private touch");
        char actual[128]={0};need(ioctl(f,EVIOCGNAME(sizeof(actual)),actual)>=0&&!strcmp(name,actual),"touch identity");
        close(f);found++;
        printf("A6L_INPUT_NODE name=%s event=%u major=%u minor=%u\n",name,number,maj,min);
    }
    closedir(d);need(found==1,"exactly one front touch device");
}
'''
s=s.replace('static void start_logger(void)',helper+'\nstatic void start_logger(void)')
s=s.replace('    dir(ROOT "/dev/dri");','    expose_touch();\n    dir(ROOT "/dev/dri");')
s=s.replace('/system/bin/a6l_surface_client','/system/bin/a6l_input_client')
s=s.replace('A6L_SURFACE_SERVICES_PASS','A6L_INPUT_SERVICES_PASS')
out.write_text(s)
print('V47 supervisor source prepared; no phone action')
