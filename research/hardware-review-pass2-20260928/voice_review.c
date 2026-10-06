// Real daemon loop; fake PCM open and clock. Never opens a sound device.
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <assert.h>
static int opens, ticks;
static int review_open(const char *p,int flags,...) {(void)p;(void)flags;++opens;errno=ENODEV;return -1;}
static int review_sleep(const struct timespec *a,struct timespec *b);
#define main daemon_main
#define open review_open
#define nanosleep review_sleep
#include "a6l_q6voiced.c"
#undef main
#undef open
#undef nanosleep
static int review_sleep(const struct timespec *a,struct timespec *b) {
    (void)a;(void)b;
    if(++ticks==12)stop_req=1;
    return 0;
}
int main(int argc,char **argv) {
    assert(argc==2);
    char *args[]={"q6voiced","-d","0","daemon",argv[1],NULL};
    int rc=daemon_main(5,args);
    printf("VOICE_RETRY active_iterations=%d open_attempts=%d loop_exit=%d\n",ticks,opens,rc);
    assert(ticks==12 && opens==5 && rc==0);
    puts("REPRODUCED retry exhaustion: remaining active iterations never attempt PCM open.");
}
