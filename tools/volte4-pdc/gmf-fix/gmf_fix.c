/* 28 Sep 2026: LD_PRELOAD shim for libqmi 1.36 qmicli --pdc-load-config.
 * qmicli-pdc.c load_config_file_from_string() calls g_free() on the pointer returned by
 * g_mapped_file_get_contents() (an mmap'd region, not malloc'd) -> musl free() segfaults.
 * This shim returns a malloc'd copy instead (the copy is freed by qmicli or leaked: < 3 MB). */
#include <stddef.h>
void *malloc(size_t);
void *memcpy(void *, const void *, size_t);
void *dlsym(void *, const char *);
#define A6L_RTLD_NEXT ((void *)-1)
typedef struct _GMappedFile GMappedFile;
char *g_mapped_file_get_contents(GMappedFile *f)
{
    static char *(*real)(GMappedFile *);
    static size_t (*len)(GMappedFile *);
    if (!real) {
        real = (char *(*)(GMappedFile *))dlsym(A6L_RTLD_NEXT, "g_mapped_file_get_contents");
        len = (size_t (*)(GMappedFile *))dlsym(A6L_RTLD_NEXT, "g_mapped_file_get_length");
    }
    char *p = real(f);
    if (!p) return p;
    size_t n = len(f);
    char *c = (char *)malloc(n ? n : 1);
    if (!c) return p;
    memcpy(c, p, n);
    return c;
}
