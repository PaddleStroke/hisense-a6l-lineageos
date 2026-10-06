// a6l_iio_ev: print IIO events (proximity near/far) of /dev/iio:deviceN for SECONDS (stk agent, 25 Sep 2026).
// usage: a6l_iio_ev /dev/iio:deviceN SECONDS
#include <errno.h>
#include <fcntl.h>
#include <linux/iio/events.h>
#include <linux/iio/types.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	int fd = open(argv[1], O_RDONLY), efd = -1, secs = atoi(argv[2]);
	if (fd < 0 || ioctl(fd, IIO_GET_EVENT_FD_IOCTL, &efd) < 0 || efd < 0) {
		printf("ERR event fd %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	time_t end = time(NULL) + secs;
	int count = 0;
	while (time(NULL) < end) {
		struct pollfd p = { .fd = efd, .events = POLLIN };
		if (poll(&p, 1, 500) <= 0)
			continue;
		struct iio_event_data ev;
		if (read(efd, &ev, sizeof(ev)) != sizeof(ev))
			continue;
		int dir = IIO_EVENT_CODE_EXTRACT_DIR(ev.id), type = IIO_EVENT_CODE_EXTRACT_CHAN_TYPE(ev.id);
		printf("A6L_STK_EVENT %d chan_type=%d dir=%s (%s) ts=%lld\n", ++count, type,
		       dir == IIO_EV_DIR_RISING ? "rising" : dir == IIO_EV_DIR_FALLING ? "falling" : "other",
		       dir == IIO_EV_DIR_RISING ? "NEAR" : "FAR", (long long)ev.timestamp);
		fflush(stdout);
	}
	printf("A6L_STK_EVENTS total=%d\n", count);
	return 0;
}
