#include <stddef.h>
typedef struct _GMappedFile GMappedFile;
GMappedFile *g_mapped_file_new(const char *, int, void **);
char *g_mapped_file_get_contents(GMappedFile *);
size_t g_mapped_file_get_length(GMappedFile *);
void g_free(void *);
int printf(const char *, ...);
void _Exit(int);
int fflush(void*);
int __libc_start_main(int (*)(int,char**,char**), int, char**);
int realmain(int c, char **v, char **en);
void _start_c(long *p){ int argc=p[0]; char **argv=(char**)(p+1); __libc_start_main(realmain, argc, argv); }
__asm__(".text\n.global _start\n_start:\n mov x0, sp\n b _start_c\n");
int realmain(int c, char **v, char **en) { void *e = 0; GMappedFile *f = g_mapped_file_new(v[1], 0, &e);
 char *p = g_mapped_file_get_contents(f); printf("len=%zu first=%02x\n", g_mapped_file_get_length(f), (unsigned char)p[0]);
 g_free(p); p = g_mapped_file_get_contents(f); printf("again first=%02x OK\n", (unsigned char)p[0]); return 0; }
