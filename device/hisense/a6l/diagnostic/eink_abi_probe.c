/* Hardware-free ABI check of the phone's own old software TCON library.
 * ReportEinkSWTconLibVersion was disassembled: stores uint32_t 2 to x0 and x1.
 * Never calls Init_Eink_SWTcon, hardware IO, waveform transforms or release.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    void *h=dlopen("libtcon_eink.so",RTLD_NOW|RTLD_LOCAL);
    if (!h) { fprintf(stderr,"A6L_EINK_DLOPEN_ERROR %s\n",dlerror());return 1; }
    const char *symbols[]={"Init_Eink_SWTcon","Release_Eink_SWTcon","SetEinkContrast","ReportEinkSWTconLibVersion"};
    for (unsigned i=0;i<sizeof(symbols)/sizeof(*symbols);i++) {
        if (!dlsym(h,symbols[i])) { fprintf(stderr,"A6L_EINK_SYMBOL_MISSING %s\n",symbols[i]);dlclose(h);return 2; }
    }
    struct { uint32_t before,major,after; } a={0xa6a6a6a6,0xffffffff,0x45454545},b=a;
    void (*version)(uint32_t*,uint32_t*)=(void(*)(uint32_t*,uint32_t*))dlsym(h,"ReportEinkSWTconLibVersion");
    version(&a.major,&b.major);
    int okay=a.before==0xa6a6a6a6 && b.before==0xa6a6a6a6 && a.after==0x45454545 && b.after==0x45454545 && a.major==2 && b.major==2;
    printf("A6L_EINK_ABI_%s version=%u.%u guards=%d init_called=0\n",okay?"PASS":"FAIL",a.major,b.major,okay);
    return dlclose(h)==0 && okay ? 0 : 3;
}
