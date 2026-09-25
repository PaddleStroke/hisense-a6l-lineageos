// a6l_vib: bounded FF_RUMBLE test for the A6L haptics input device.
// usage: a6l_vib <strength 0..65535> <ms 1..1000> [devname=spmi_haptics]
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int find_dev(const char *want, char *path, size_t len)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	if (!d) return -1;
	while ((e = readdir(d))) {
		char name[128] = {0};
		int fd;
		if (strncmp(e->d_name, "event", 5)) continue;
		snprintf(path, len, "/dev/input/%s", e->d_name);
		fd = open(path, O_RDWR);
		if (fd < 0) continue;
		ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
		if (!strcmp(name, want)) { closedir(d); return fd; }
		close(fd);
	}
	closedir(d);
	return -1;
}

int main(int argc, char **argv)
{
	char path[300];
	struct ff_effect eff;
	struct input_event ev;
	long strength, ms;
	int fd;

	if (argc < 3) { fprintf(stderr, "usage: %s <0..65535> <1..1000 ms> [devname]\n", argv[0]); return 2; }
	strength = strtol(argv[1], NULL, 0);
	ms = strtol(argv[2], NULL, 0);
	if (strength < 0 || strength > 65535 || ms < 1 || ms > 1000) { fprintf(stderr, "A6L_VIB_FAIL out of range\n"); return 2; }
	fd = find_dev(argc > 3 ? argv[3] : "spmi_haptics", path, sizeof(path));
	if (fd < 0) { fprintf(stderr, "A6L_VIB_FAIL no haptics input device\n"); return 1; }
	memset(&eff, 0, sizeof(eff));
	eff.type = FF_RUMBLE;
	eff.id = -1;
	eff.u.rumble.strong_magnitude = (unsigned short)strength;
	eff.replay.length = (unsigned short)ms;
	if (ioctl(fd, EVIOCSFF, &eff) < 0) { perror("A6L_VIB_FAIL EVIOCSFF"); return 1; }
	memset(&ev, 0, sizeof(ev));
	ev.type = EV_FF; ev.code = eff.id; ev.value = 1;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) { perror("A6L_VIB_FAIL play"); return 1; }
	usleep((ms + 50) * 1000);
	ev.value = 0;
	if (write(fd, &ev, sizeof(ev)) < 0) perror("stop");
	ioctl(fd, EVIOCRMFF, eff.id);
	close(fd);
	printf("A6L_VIB_DONE dev=%s strength=%ld ms=%ld\n", path, strength, ms);
	return 0;
}
