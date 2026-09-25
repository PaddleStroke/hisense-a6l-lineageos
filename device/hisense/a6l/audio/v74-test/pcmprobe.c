// SPDX-License-Identifier: GPL-2.0-only
/*
 * pcmprobe: open an ALSA PCM node exactly like tinyalsa (O_RDWR|O_NONBLOCK) and print the REAL errno
 * (tinyalsa's "Device does not exist." / "cannot open device" messages hide it). Then HW_REFINE (what
 * tinypcminfo does) and, with "hw", a 48 kHz/S16_LE/2ch HW_PARAMS + HW_FREE (no PREPARE/START) (no data is ever written).
 * usage: pcmprobe /dev/snd/pcmC0D0p [hw]
 * Written 23 Sep 2026 for the A6L port (audio2). No sound can be produced by this tool.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

static void any(struct snd_pcm_hw_params *p)
{
	int n;

	memset(p, 0, sizeof(*p));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
		memset(&p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0xff, sizeof(struct snd_mask));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
		p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 0;
		p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = ~0U;
	}
	p->rmask = ~0U;
	p->info = ~0U;
}

static struct snd_interval *iv(struct snd_pcm_hw_params *p, int n)
{
	return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void fix(struct snd_pcm_hw_params *p, int n, unsigned int v)
{
	iv(p, n)->min = iv(p, n)->max = v;
	iv(p, n)->integer = 1;
}

int main(int argc, char **argv)
{
	struct snd_pcm_hw_params p;
	struct snd_pcm_info info;
	int fd, r;

	if (argc < 2) {
		fprintf(stderr, "usage: %s /dev/snd/pcmCxDyp|c [hw]\n", argv[0]);
		return 2;
	}
	errno = 0;
	fd = open(argv[1], O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		int e = errno;
		printf("A6L_PCMPROBE %s open errno=%d (%s)\n", argv[1], e, strerror(e));
		return 1;
	}
	printf("A6L_PCMPROBE %s open OK\n", argv[1]);
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, SNDRV_PCM_IOCTL_INFO, &info) == 0)
		printf("A6L_PCMPROBE info id='%s' name='%s' subname='%s' stream=%d subdevices=%u/%u\n", info.id,
		       info.name, info.subname, info.stream, info.subdevices_avail, info.subdevices_count);
	any(&p);
	r = ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p);
	if (r) {
		printf("A6L_PCMPROBE HW_REFINE errno=%d (%s)\n", errno, strerror(errno));
	} else {
		printf("A6L_PCMPROBE HW_REFINE OK rate %u-%u ch %u-%u period_bytes %u-%u periods %u-%u buffer_bytes %u-%u fmt0=0x%08x\n",
		       iv(&p, SNDRV_PCM_HW_PARAM_RATE)->min, iv(&p, SNDRV_PCM_HW_PARAM_RATE)->max,
		       iv(&p, SNDRV_PCM_HW_PARAM_CHANNELS)->min, iv(&p, SNDRV_PCM_HW_PARAM_CHANNELS)->max,
		       iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->min, iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->max,
		       iv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->min, iv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->max,
		       iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->min, iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->max,
		       p.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0]);
	}
	if (argc > 2 && !strcmp(argv[2], "hw")) {
		any(&p);
		p.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = 1U << SNDRV_PCM_ACCESS_RW_INTERLEAVED;
		p.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = 1U << SNDRV_PCM_FORMAT_S16_LE;
		p.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = 1U << SNDRV_PCM_SUBFORMAT_STD;
		fix(&p, SNDRV_PCM_HW_PARAM_RATE, 48000);
		fix(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
		fix(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 960);
		fix(&p, SNDRV_PCM_HW_PARAM_PERIODS, 4);
		r = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p);
		printf("A6L_PCMPROBE HW_PARAMS 48k/S16/2ch/960x4 -> %s%d (%s)\n", r ? "errno=" : "", r ? errno : 0,
		       r ? strerror(errno) : "OK");
		if (!r)
			ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE);	/* no PREPARE: would power the DAPM path / start the AFE port */
	}
	close(fd);
	return 0;
}
