// SPDX-License-Identifier: MIT
/*
 * a6l-q6voiced (kvoice agent, 24 Sep 2026): Android/NDK port of the idea of postmarketOS q6voiced
 * (https://gitlab.postmarketos.org/postmarketOS/q6voiced, MIT). Voice call audio flows modem <-> ADSP (CVD) <-> AFE
 * ports; nothing flows through the CPU, but the kernel q6voice driver only starts the MVM/CVP session while BOTH the
 * VoiceMMode1 playback and capture PCMs are open (+prepared/started). This tool holds them open.
 * No dbus/ModemManager on Android: it is driven by a system property (daemon) or used one-shot (attended tests).
 * Raw ALSA ioctls (no tinyalsa/alsa-lib), static-linkable with the NDK.
 *
 *   a6l-q6voiced [-c card] [-d device] [-r] hold <seconds>     open both, start, hold N s, close (test)
 *   a6l-q6voiced [-c card] [-d device] [-r] daemon [prop]      follow prop (default vendor.a6l.voice.active) 1/0
 *   a6l-q6voiced [-c card] routes <0|1>                         only set/clear the q6voice DSP routes
 *   -d default: the pcm named "VoiceMMode1" in /proc/asound/pcm; -c default 0.
 *   -r: also set the DSP routes (LPI_MI2S_RX_0 Voice Mixer VoiceMMode1, VoiceMMode1 Capture Mixer LPI_MI2S_TX_3)
 *       on open and clear them on close. Analog (codec) routing stays with a6l-audio-route / mixer_paths.
 * Log lines start with A6L_Q6VOICED. Exit 0 = ok.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define RATE 8000
#define PERIOD_FRAMES 1024	/* 2048 bytes: q6voice_dai_hardware period_bytes 2048..4096 */
#define PERIODS 2

static volatile sig_atomic_t stop_req;
static void on_sig(int s) { (void)s; stop_req = 1; }

static const char *ROUTE_RX = "LPI_MI2S_RX_0 Voice Mixer VoiceMMode1";
static const char *ROUTE_TX = "VoiceMMode1 Capture Mixer LPI_MI2S_TX_3";

static int find_voice_dev(int card)
{
	FILE *f = fopen("/proc/asound/pcm", "r");
	char line[256];
	int c, d, found = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%d-%d:", &c, &d) == 2 && c == card && strstr(line, "VoiceMMode1")) {
			found = d;
			break;
		}
	}
	fclose(f);
	return found;
}

static int ctl_set(int card, const char *name, long v)
{
	char path[64];
	struct snd_ctl_elem_value ev;
	int fd, ret;

	snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "A6L_Q6VOICED ctl open %s: %s\n", path, strerror(errno));
		return -1;
	}
	memset(&ev, 0, sizeof(ev));
	ev.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strncpy((char *)ev.id.name, name, sizeof(ev.id.name) - 1);
	ev.value.integer.value[0] = v;
	ret = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
	if (ret < 0)
		fprintf(stderr, "A6L_Q6VOICED ctl '%s' = %ld: %s\n", name, v, strerror(errno));
	else
		printf("A6L_Q6VOICED ctl '%s' = %ld\n", name, v);
	close(fd);
	return ret < 0 ? -1 : 0;
}

static int routes(int card, int on)
{
	int r = 0;
	r |= ctl_set(card, ROUTE_RX, on);
	r |= ctl_set(card, ROUTE_TX, on);
	return r;
}

static void mask_set(struct snd_interval *unused, struct snd_mask *m, unsigned int bit)
{
	(void)unused;
	memset(m, 0, sizeof(*m));
	m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void iv_set(struct snd_interval *i, unsigned int v)
{
	memset(i, 0, sizeof(*i));
	i->min = i->max = v;
	i->integer = 1;
}

static void hw_any(struct snd_pcm_hw_params *p)
{
	int n;

	memset(p, 0, sizeof(*p));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
		memset(&p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0xff, sizeof(struct snd_mask));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
		struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
		i->min = 0;
		i->max = ~0u;
	}
	p->rmask = ~0u;
	p->info = ~0u;
}

static int pcm_open_start(int card, int dev, int capture)
{
	char path[64];
	struct snd_pcm_hw_params hp;
	struct snd_pcm_sw_params sp;
	int fd;
	const char *what = capture ? "capture" : "playback";

	snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%d%c", card, dev, capture ? 'c' : 'p');
	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "A6L_Q6VOICED open %s: %s\n", path, strerror(errno));
		return -1;
	}
	hw_any(&hp);
	mask_set(NULL, &hp.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK], SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_set(NULL, &hp.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK], SNDRV_PCM_FORMAT_S16_LE);
	mask_set(NULL, &hp.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK], SNDRV_PCM_SUBFORMAT_STD);
	iv_set(&hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], 1);
	iv_set(&hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], RATE);
	iv_set(&hp.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], PERIOD_FRAMES);
	iv_set(&hp.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], PERIODS);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
		fprintf(stderr, "A6L_Q6VOICED %s HW_PARAMS: %s\n", what, strerror(errno));
		goto err;
	}
	memset(&sp, 0, sizeof(sp));
	sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sp.period_step = 1;
	sp.avail_min = 1;
	sp.start_threshold = 1;
	sp.stop_threshold = ~0ul >> 1;	/* never auto-stop on "xrun": no data ever flows through the CPU */
	sp.boundary = 0;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) < 0)
		fprintf(stderr, "A6L_Q6VOICED %s SW_PARAMS: %s (ignored)\n", what, strerror(errno));
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		fprintf(stderr, "A6L_Q6VOICED %s PREPARE: %s\n", what, strerror(errno));
		goto err;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0)
		fprintf(stderr, "A6L_Q6VOICED %s START: %s (ignored, prepare is enough on older setups)\n", what, strerror(errno));
	printf("A6L_Q6VOICED %s %s open+prepared\n", what, path);
	return fd;
err:
	close(fd);
	return -1;
}

struct voice { int card, dev, tx, rx, set_routes; };

static int voice_open(struct voice *v)
{
	if (v->tx >= 0)
		return 0;
	if (v->set_routes && routes(v->card, 1))
		fprintf(stderr, "A6L_Q6VOICED warning: route controls not all set\n");
	/* capture (tx) first, as q6voiced: q6voice starts the path when the second direction opens */
	v->tx = pcm_open_start(v->card, v->dev, 1);
	if (v->tx < 0)
		goto err;
	v->rx = pcm_open_start(v->card, v->dev, 0);
	if (v->rx < 0)
		goto err;
	printf("A6L_Q6VOICED OPEN card %d dev %d\n", v->card, v->dev);
	fflush(stdout);
	return 0;
err:
	if (v->tx >= 0)
		close(v->tx);
	v->tx = v->rx = -1;
	if (v->set_routes)
		routes(v->card, 0);
	printf("A6L_Q6VOICED OPEN_FAIL (see kernel log: A6L_Q6VOICE / q6voice / q6cvp / q6mvm)\n");
	fflush(stdout);
	return -1;
}

static void voice_close(struct voice *v)
{
	if (v->tx < 0)
		return;
	ioctl(v->rx, SNDRV_PCM_IOCTL_DROP);
	ioctl(v->tx, SNDRV_PCM_IOCTL_DROP);
	close(v->rx);
	close(v->tx);
	v->tx = v->rx = -1;
	if (v->set_routes)
		routes(v->card, 0);
	printf("A6L_Q6VOICED CLOSED\n");
	fflush(stdout);
}

static int prop_active(const char *prop)
{
#ifdef __ANDROID__
	char val[PROP_VALUE_MAX] = "";
	__system_property_get(prop, val);
	return val[0] == '1';
#else
	/* host test build: the "property" is a file */
	FILE *f = fopen(prop, "r");
	int c = f ? fgetc(f) : 0;
	if (f)
		fclose(f);
	return c == '1';
#endif
}

static void usage(void)
{
	fprintf(stderr, "usage: a6l-q6voiced [-c card] [-d dev] [-r] hold <sec> | daemon [prop] | routes <0|1>\n");
	exit(2);
}

int main(int argc, char **argv)
{
	struct voice v = { .card = 0, .dev = -1, .tx = -1, .rx = -1, .set_routes = 0 };
	int opt;

	while ((opt = getopt(argc, argv, "c:d:r")) != -1) {
		switch (opt) {
		case 'c': v.card = atoi(optarg); break;
		case 'd': v.dev = atoi(optarg); break;
		case 'r': v.set_routes = 1; break;
		default: usage();
		}
	}
	if (optind >= argc)
		usage();
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (!strcmp(argv[optind], "routes")) {
		if (optind + 1 >= argc)
			usage();
		return routes(v.card, atoi(argv[optind + 1])) ? 1 : 0;
	}
	if (v.dev < 0)
		v.dev = find_voice_dev(v.card);
	if (v.dev < 0) {
		fprintf(stderr, "A6L_Q6VOICED no VoiceMMode1 pcm on card %d (/proc/asound/pcm)\n", v.card);
		return 3;
	}
	if (!strcmp(argv[optind], "hold")) {
		int sec = optind + 1 < argc ? atoi(argv[optind + 1]) : 10;
		struct timespec ts = { 0, 200 * 1000 * 1000 };
		int i;

		if (voice_open(&v))
			return 1;
		for (i = 0; i < sec * 5 && !stop_req; i++)
			nanosleep(&ts, NULL);
		voice_close(&v);
		return 0;
	}
	if (!strcmp(argv[optind], "daemon")) {
		const char *prop = optind + 1 < argc ? argv[optind + 1] : "vendor.a6l.voice.active";
		struct timespec ts = { 0, 250 * 1000 * 1000 };
		int fails = 0;

		printf("A6L_Q6VOICED daemon card %d dev %d prop %s\n", v.card, v.dev, prop);
		while (!stop_req) {
			int want = prop_active(prop);

			if (want && v.tx < 0 && fails < 5) {
				if (voice_open(&v))
					fails++;
			} else if (!want) {
				fails = 0;
				voice_close(&v);
			}
			nanosleep(&ts, NULL);
		}
		voice_close(&v);
		return 0;
	}
	usage();
	return 2;
}
