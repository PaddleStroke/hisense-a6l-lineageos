/* Diskless VM helper: create init-style control sockets (ANDROID_SOCKET_<name>)
 * and exec a genuine Android daemon that expects init to have provided them.
 * usage: a6l_socket_exec name[:octal-mode] ... -- /path/to/daemon [args] */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && strcmp(argv[i], "--"); i++) {
        char name[64]; unsigned mode = 0666;
        const char *colon = strchr(argv[i], ':');
        size_t n = colon ? (size_t)(colon - argv[i]) : strlen(argv[i]);
        if (n == 0 || n >= sizeof(name)) return 100;
        memcpy(name, argv[i], n); name[n] = 0;
        if (colon) mode = (unsigned)strtoul(colon + 1, NULL, 8);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof(a.sun_path), "/dev/socket/%s", name);
        unlink(a.sun_path);
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || chmod(a.sun_path, mode)) { perror(name); return 101; }
        char key[96], val[16];
        snprintf(key, sizeof(key), "ANDROID_SOCKET_%s", name);
        snprintf(val, sizeof(val), "%d", s);
        setenv(key, val, 1);
    }
    if (i + 1 >= argc) return 102;
    execv(argv[i + 1], &argv[i + 1]);
    perror("execv"); return 103;
}
