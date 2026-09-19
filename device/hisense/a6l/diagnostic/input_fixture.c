/* Diskless emulator only. Synthesize two-slot touch into real kernel evdev. */
#define _GNU_SOURCE
#include <linux/uinput.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static void need(int ok,const char *s){if(!ok){perror(s);exit(1);}}
static void emit(int fd,unsigned short type,unsigned short code,int value){struct input_event e={.type=type,.code=code,.value=value};need(write(fd,&e,sizeof(e))==sizeof(e),"input write");}
static void abs_axis(int fd,int code,int max){need(!ioctl(fd,UI_SET_ABSBIT,code),"abs bit");struct uinput_abs_setup a={.code=code,.absinfo={.minimum=0,.maximum=max}};need(!ioctl(fd,UI_ABS_SETUP,&a),"abs setup");}
static void slot(int fd,int s,int id,int x,int y){emit(fd,EV_ABS,ABS_MT_SLOT,s);emit(fd,EV_ABS,ABS_MT_TRACKING_ID,id);if(id>=0){emit(fd,EV_ABS,ABS_MT_POSITION_X,x);emit(fd,EV_ABS,ABS_MT_POSITION_Y,y);}}
int main(void){
    setbuf(stdout,NULL);alarm(150);
    need(access("/sys/firmware/devicetree/base/model",R_OK)==0,"DT model");
    char model[128]={0};int m=open("/sys/firmware/devicetree/base/model",O_RDONLY);need(m>=0,"model open");need(read(m,model,sizeof(model)-1)>0,"model read");close(m);
    need(strstr(model,"virt")!=NULL,"QEMU virt only");
    need(!mknod("/dev/input-fixture-uinput",S_IFCHR|0600,makedev(10,223))||errno==EEXIST,"uinput node");
    int fd=open("/dev/input-fixture-uinput",O_RDWR|O_CLOEXEC);need(fd>=0,"uinput open");
    need(!ioctl(fd,UI_SET_EVBIT,EV_KEY)&&!ioctl(fd,UI_SET_KEYBIT,BTN_TOUCH)&&!ioctl(fd,UI_SET_EVBIT,EV_ABS),"input bits");
    need(!ioctl(fd,UI_SET_PROPBIT,INPUT_PROP_DIRECT),"direct property");
    abs_axis(fd,ABS_MT_SLOT,1);abs_axis(fd,ABS_MT_TRACKING_ID,65535);abs_axis(fd,ABS_MT_POSITION_X,1079);abs_axis(fd,ABS_MT_POSITION_Y,2339);
    struct uinput_setup setup={.id={.bustype=BUS_VIRTUAL,.vendor=0xa647,.product=1,.version=1}};strcpy(setup.name,"A6L Input Fixture");
    need(!ioctl(fd,UI_DEV_SETUP,&setup)&&!ioctl(fd,UI_DEV_CREATE),"create touch");
    puts("A6L_INPUT_FIXTURE_CREATED");
    while(access("/tmp/a6l-v47/root/logs/input-ready",F_OK))usleep(100000);
    sleep(2);
    // Four corners/quadrants, then a two-finger gesture with distinct coordinates.
    int points[4][2]={{180,300},{900,300},{180,2040},{900,2040}};
    for(int i=0;i<4;i++){
        slot(fd,0,100+i,points[i][0],points[i][1]);emit(fd,EV_KEY,BTN_TOUCH,1);emit(fd,EV_SYN,SYN_REPORT,0);usleep(200000);
        slot(fd,0,-1,0,0);emit(fd,EV_KEY,BTN_TOUCH,0);emit(fd,EV_SYN,SYN_REPORT,0);usleep(200000);
    }
    slot(fd,0,200,300,900);slot(fd,1,201,700,1500);emit(fd,EV_KEY,BTN_TOUCH,1);emit(fd,EV_SYN,SYN_REPORT,0);usleep(200000);
    for(int i=1;i<=4;i++) {emit(fd,EV_ABS,ABS_MT_SLOT,0);emit(fd,EV_ABS,ABS_MT_POSITION_X,300+20*i);emit(fd,EV_ABS,ABS_MT_SLOT,1);emit(fd,EV_ABS,ABS_MT_POSITION_X,700-20*i);emit(fd,EV_SYN,SYN_REPORT,0);usleep(100000);}
    slot(fd,1,-1,0,0);emit(fd,EV_SYN,SYN_REPORT,0);usleep(200000);
    slot(fd,0,-1,0,0);emit(fd,EV_KEY,BTN_TOUCH,0);emit(fd,EV_SYN,SYN_REPORT,0);
    puts("A6L_INPUT_FIXTURE_SENT");sleep(4);
    int f=open("/tmp/a6l-v47/root/logs/finish",O_CREAT|O_WRONLY|O_CLOEXEC,0600);need(f>=0,"finish");close(f);
    sleep(4);need(!ioctl(fd,UI_DEV_DESTROY),"destroy input");close(fd);puts("A6L_INPUT_FIXTURE_PASS");return 0;
}
