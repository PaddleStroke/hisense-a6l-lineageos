// Exercise real grab and input drain functions; regular files stand in for evdev.
#define main dualux_daemon_main
#include "a6l_dualux.c"
#undef main
#include <assert.h>
int main(int argc,char **argv) {
    assert(argc==2);
    struct dx_cfg cfg;dx_default_cfg(&cfg);dx_init(&S,&cfg,DX_EINK,1);
    ui=-1; // Actual state after uinput_open() failure.
    dev_fd[DEV_POWER]=open(argv[1],O_RDONLY|O_CLOEXEC);
    assert(dev_fd[DEV_POWER]>=0);
    apply_grabs();
    printf("NO_UINPUT ui=%d power_grabbed=%d\n",ui,dev_grabbed[DEV_POWER]);
    assert(dev_grabbed[DEV_POWER]==1);
    close(dev_fd[DEV_POWER]);dev_fd[DEV_POWER]=-1;

    // SYN_DROPPED discards an unread release; the EVIOCGKEY-resync path is absent.
    FILE *f=fopen(argv[1],"wb");assert(f);
    struct input_event events[2]={{.type=EV_SYN,.code=SYN_DROPPED},{.type=EV_SYN,.code=SYN_REPORT}};
    assert(fwrite(events,sizeof(events),1,f)==1);fclose(f);
    dx_init(&S,&cfg,DX_EINK,1);
    (void)dx_power_key(&S,1,0.0);
    struct dx_out held=dx_tick(&S,1.0);
    assert(held.power_down && S.pw_injected);
    dev_fd[DEV_POWER]=open(argv[1],O_RDONLY|O_CLOEXEC);assert(dev_fd[DEV_POWER]>=0);
    drain(DEV_POWER);
    printf("SYN_DROPPED power_down=%d injected_down=%d\n",S.pw_down,S.pw_injected);
    assert(S.pw_down && S.pw_injected);
    close(dev_fd[DEV_POWER]);
    puts("REPRODUCED unavailable-uinput grab and lost-key-state recovery defects.");
}
