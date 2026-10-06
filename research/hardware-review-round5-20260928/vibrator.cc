// Full, unchanged InputFFDevice constructor/play/isPresent with fake evdev calls.
#include <linux/input.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <mutex>
#include <type_traits>
#include <string>
#define INVALID_VALUE -1
#define CUSTOM_DATA_LEN 3
#define NAME_BUF_SIZE 32
#define ALOGE(...) ((void)0)
#define ALOGD(...) ((void)0)
#define ALOGI(...) ((void)0)
#define test_bit(bit,array) ((array)[(bit)/8] & (1<<((bit)%8)))
static const char* fixture_dir;
static const char* fixture_name;
static int cap_queries,uploads,writes;
static DIR* fake_opendir(const char*) {return ::opendir(fixture_dir);}
static int fake_open(const char*,int){return 99;}
static int fake_close(int){return 0;}
static int property_get_int32(const char*,int){return 1;}
template<typename T> static int fake_ioctl(int,unsigned long cmd,T arg){
 if constexpr(std::is_pointer_v<T>){
  if(cmd==EVIOCGNAME(NAME_BUF_SIZE)){strcpy((char*)arg,fixture_name);return 0;}
  if(cmd==EVIOCGBIT(EV_FF,FF_CNT/8)){
   ++cap_queries;auto* b=(unsigned char*)arg;
   b[FF_CONSTANT/8]|=1<<(FF_CONSTANT%8);b[FF_GAIN/8]|=1<<(FF_GAIN%8);return 0;
  }
  if(cmd==EVIOCSFF){++uploads;((ff_effect*)arg)->id=0;return 0;}
 }
 if(cmd==EVIOCRMFF)return 0;
 assert(false);return -1;
}
static ssize_t fake_write(int,const void*,size_t n){++writes;return n;}
struct InputFFDevice {
 int mVibraFd;bool mSupportGain,mSupportEffects,mSupportExternalControl,mInExternalControl;
 int16_t mCurrAppId,mCurrMagnitude;std::mutex mtx;
 InputFFDevice();bool isPresent();int play(int,uint32_t,long*);
};
#include "soc.inc"
#define opendir fake_opendir
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define write fake_write
#include "vibrator_methods.inc"
#undef opendir
#undef open
#undef close
#undef ioctl
#undef write
int main(int argc,char** argv){
 assert(argc==2);fixture_dir=argv[1];fixture_name="a6l_gpio_vibrator";
 InputFFDevice actual;assert(!actual.isPresent());assert(actual.play(-1,400,nullptr)==0);
 assert(cap_queries==0 && uploads==0 && writes==0);
 printf("F49 driver_name=%s present=0 on_400ms_success=1 capability_queries=0 FF_uploads=0 FF_writes=0\n",fixture_name);
 fixture_name="qti-haptics";
 InputFFDevice control;assert(control.isPresent());assert(control.play(-1,400,nullptr)==0);
 assert(cap_queries==1 && uploads==1 && writes==1);
 printf("F49 control_name=%s present=1 FF_uploads=1 FF_writes=1\n",fixture_name);
}
