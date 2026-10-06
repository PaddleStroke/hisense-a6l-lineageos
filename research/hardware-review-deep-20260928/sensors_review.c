// Actual motion engine, deterministic sample times; no hardware access.
// r5 pass2 (29 Sep 2026): converted to FIXED expectations after F30/F31/F32 (hals/sensors/stk3338/a6l_motion.c).
#define _GNU_SOURCE
#include "a6l_motion.c"
#include <assert.h>
static void init_acc(struct a6l_motion *m) {
    a6l_motion_init(m);
    struct a6l_iio *d=&m->d[A6L_ACC];
    d->rec=24; d->nch=4; d->has_ts=1; d->scale=1.; d->native_ns=5000000.; d->epoch=1;
    for(int i=0;i<4;i++) d->ch[i]=(struct a6l_chan){.bytes=i==3?8:4,.bits=32,.off=i==3?16:i*4,.axis=i};
    a6l_tsmap_reset(&d->tm,32768.);
    m->h[0].enabled=1; m->h[0].period=5000000;
}
static void rec(uint8_t *b,int i) { uint32_t tick=(uint32_t)(i*32768LL/200); memset(b,0,24); memcpy(b+16,&tick,4); }
static void feed(struct a6l_motion *m,int start,int n,int64_t now) {
    uint8_t b[64*24];
    for(int i=0;i<n;i++) rec(b+i*24,start+i);
    process(m,A6L_ACC,b,n,now);
}
int main(void) {
    struct a6l_motion *m=calloc(1,sizeof(*m)); assert(m);
    init_acc(m);
    for(int i=0;i<256;i++) feed(m,i,1,10000000000LL+i*5000000LL);
    int prompt=m->q.n;
    // Reviewer's case at the process() level: 4 x 64 chunks with near-identical arrival times. Decimation now runs
    // on acquisition ticks, so no valid sample is discarded (timestamps of later chunks are still compressed here).
    init_acc(m);
    for(int i=0;i<4;i++) feed(m,i*64,64,11275000000LL+i*100000LL);
    int chunked=m->q.n;
    // The real drain: all 256 waiting scans are read before the anchor is chosen.
    init_acc(m);
    char path[]="/tmp/a6l-review-backlog-XXXXXX"; int fd=mkstemp(path); assert(fd>=0); unlink(path);
    for(int i=0;i<256;i++){ uint8_t b[24]; rec(b,i); assert(write(fd,b,24)==24); }
    lseek(fd,0,SEEK_SET); m->d[A6L_ACC].fd=fd; m->d[A6L_ACC].on=1;
    a6l_motion_service(m);
    int n=m->q.n; int64_t first=m->q.ev[m->q.head].timestamp, last=m->q.ev[(m->q.head+n-1)%A6L_EVQ].timestamp;
    printf("SENSOR_BACKLOG prompt=%d chunked_process=%d drained=%d scans=256 span_error_ms=%.3f\n",
        prompt,chunked,n,((last-first)-255*5000000LL)/1e6);
    assert(prompt==256 && chunked==256 && n==256 && llabs((last-first)-255*5000000LL)<100000);
    close(fd); m->d[A6L_ACC].fd=-1; m->d[A6L_ACC].on=0;
    // F31: disabling purges the handle's queued samples.
    assert(a6l_motion_activate(m,A6L_H_ACCEL,0)==0);
    sensors_event_t e;
    int left=a6l_motion_pop(m,&e,1);
    printf("SENSOR_DISABLED enabled=%d delivered_after_disable=%d\n",m->h[0].enabled,left);
    assert(!m->h[0].enabled && left==0);
    // F31: overflow keeps the FLUSH_COMPLETE.
    memset(&m->q,0,sizeof(m->q));
    e=(sensors_event_t){.type=SENSOR_TYPE_META_DATA}; e.meta_data.what=META_DATA_FLUSH_COMPLETE;
    e.meta_data.sensor=A6L_H_ACCEL; a6l_evq_push(&m->q,&e);
    e=(sensors_event_t){.sensor=A6L_H_GYRO,.type=SENSOR_TYPE_GYROSCOPE};
    for(int i=0;i<A6L_EVQ;i++) a6l_evq_push(&m->q,&e);
    int metas=0; while(a6l_evq_pop(&m->q,&e,1)) metas+=e.type==SENSOR_TYPE_META_DATA;
    printf("SENSOR_FLUSH_OVERFLOW flush_completions=%d dropped=%ld\n",metas,m->q.dropped);
    assert(metas==1 && m->q.dropped==1);
    // F32: slow yaw (0.04 rad/s about gravity) with a still accelerometer and no magnetometer: not learned as bias.
    memset(m,0,sizeof(*m)); double w[3]={0,0,0.04}, a[3]={0,0,9.80665};
    for(int i=0;i<=220;i++){ a6l_ring_add(&m->acc_ring,i*5000000LL,a); a6l_gyro_bias_feed(m,w,i*5000000LL); a6l_gyro_cal_eval(m,i*5000000LL); }
    printf("GYRO_SLOW_ROTATION true_z=0.04 learned_bias_z=%.6f corrected_z=%.6f valid=%d full=%d\n",m->gb[2],w[2]-m->gb[2],m->gb_valid,m->gb_full);
    assert(fabs(w[2]-m->gb[2]-0.04)<1e-6 && !m->gb_full);
    free(m);
}
