/* r6d (30 Sep 2026, docs/rom-r6d-20260930.md): offline load test of the vendor Mesa EGL/GLES set for one ABI.
 * Built with the NDK for armv7a and aarch64, run under qemu-arm / qemu-aarch64 (user mode) against the files extracted
 * from the BUILT vendor/system images (real bionic linker + libc): dlopen each library (RTLD_NOW = every relocation
 * resolved, constructors run), then what the Android EGL loader and zygote preload do first: eglGetDisplay(DEFAULT)
 * through libEGL_mesa, eglGetProcAddress + a GLES entry point from libGLESv2_mesa.
 * usage: a6l_egl_load <libEGL_mesa.so> <libGLESv1_CM_mesa.so> <libGLESv2_mesa.so>  -> A6L_EGL_LOAD_OK <bits> */
#include <dlfcn.h>
#include <stdio.h>
typedef void *(*get_display_t)(void *);
typedef void *(*get_proc_t)(const char *);
int main(int argc, char **argv) {
    void *h[3];
    if (argc != 4) { fprintf(stderr, "usage\n"); return 2; }
    for (int i = 0; i < 3; i++) {
        h[i] = dlopen(argv[i + 1], RTLD_NOW | RTLD_LOCAL);
        if (!h[i]) { printf("A6L_EGL_LOAD_FAIL dlopen %s: %s\n", argv[i + 1], dlerror()); return 1; }
        printf("dlopen ok %s\n", argv[i + 1]);
    }
    get_display_t gd = (get_display_t)dlsym(h[0], "eglGetDisplay");
    get_proc_t gp = (get_proc_t)dlsym(h[0], "eglGetProcAddress");
    void *draw = dlsym(h[2], "glDrawArrays"), *str = dlsym(h[2], "glGetString"), *gl1 = dlsym(h[1], "glLoadIdentity");
    if (!gd || !gp || !draw || !str || !gl1) { printf("A6L_EGL_LOAD_FAIL missing entry point\n"); return 1; }
    void *dpy = gd((void *)0);
    void *pa = gp("glDrawArrays");
    printf("eglGetDisplay(EGL_DEFAULT_DISPLAY)=%s eglGetProcAddress(glDrawArrays)=%s\n", dpy ? "non-null" : "NULL", pa ? "non-null" : "NULL");
    if (!dpy || !pa) { printf("A6L_EGL_LOAD_FAIL\n"); return 1; }
    printf("A6L_EGL_LOAD_OK %d-bit\n", (int)(sizeof(void *) * 8));
    return 0;
}
