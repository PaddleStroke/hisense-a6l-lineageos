// SPDX-License-Identifier: MIT
/*
 * r5 review fixes (28 Sep 2026): host tests of a6l-q6voiced (F6 mute socket, F7 speakerphone RX port,
 * pass2 F19 open retry for the whole call; btcall 29 Sep 2026: BT headset call bridge).
 * The daemon source is included with main() compiled out and the ALSA control write replaced by a fake.
 * Build/run: gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -O1 -g -fsanitize=address,undefined -pthread \
 *            -o /tmp/q6voiced-tests tests/q6voiced_tests.c && /tmp/q6voiced-tests
 */
#define A6L_Q6VOICED_NO_MAIN
#include "../a6l_q6voiced.c"

#include <pthread.h>

static int g_pass, g_fail;
#define EXPECT(c) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- fake ALSA control writes */
static char w_name[32][48];
static long w_val[32];
static int w_n;
static const char *fail_name;
static int fail_err;

static int fake_write(int card, const char *name, long v)
{
	(void)card;
	if (fail_name && !strcmp(name, fail_name))
		return -fail_err;
	if (w_n < 32) {
		snprintf(w_name[w_n], sizeof(w_name[0]), "%s", name);
		w_val[w_n++] = v;
	}
	return 0;
}

static void reset(void) { w_n = 0; fail_name = NULL; fail_err = 0; }

static void test_mute_cmd(void)
{
	struct voice v = { .card = 0, .dev = -1, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	char r[128];

	reset();
	mute_want = -1;
	handle_cmd(&v, "getmute", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=0"));
	handle_cmd(&v, "mute 1", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=1"));
	EXPECT(w_n == 1 && !strcmp(w_name[0], "VoiceMMode1 TX Mute") && w_val[0] == 1);
	EXPECT(mute_want == 1);
	/* DSP rejects the unmute: error reported, state unchanged */
	fail_name = "VoiceMMode1 TX Mute";
	fail_err = EIO;
	handle_cmd(&v, "mute 0", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 5 ", 6));
	EXPECT(mute_want == 1);
	handle_cmd(&v, "getmute", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=1"));
	/* kernel without the tx-mute patch: ENOENT (HAL -> REQUEST_NOT_SUPPORTED) */
	fail_err = ENOENT;
	handle_cmd(&v, "mute 1", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 2 ", 6));
	reset();
	handle_cmd(&v, "mute 0", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=0") && mute_want == 0);
	handle_cmd(&v, "mute 2", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 22", 6));
	handle_cmd(&v, "mute 1x", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 22", 6));
	handle_cmd(&v, "status", r, sizeof(r));
	EXPECT(!strcmp(r, "OK open=0 rx=earpiece mute=0"));
}

/* r5 review round4 F37: in-call volume -> "VoiceMMode1 RX Volume Step" */
static void test_volume_cmd(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	char r[128];
	int i, found;

	reset();
	vol_want = -1;
	handle_cmd(&v, "getvolume", r, sizeof(r));
	EXPECT(!strcmp(r, "OK volume=-1"));
	handle_cmd(&v, "volume 1", r, sizeof(r));
	EXPECT(!strcmp(r, "OK volume=5"));
	EXPECT(w_n == 1 && !strcmp(w_name[0], "VoiceMMode1 RX Volume Step") && w_val[0] == 5 && vol_want == 5);
	handle_cmd(&v, "volume 0.5", r, sizeof(r));
	EXPECT(!strcmp(r, "OK volume=3") && w_val[1] == 3);
	handle_cmd(&v, "volume 0.2", r, sizeof(r));
	EXPECT(!strcmp(r, "OK volume=1"));
	handle_cmd(&v, "volume 0", r, sizeof(r));
	EXPECT(!strcmp(r, "OK volume=0") && vol_want == 0);	/* lowest calibrated level, not a mute */
	/* monotonic over Android's range */
	{
		int last = -1, mono = 1;
		double f;

		for (f = 0.0; f <= 1.0001; f += 0.05) {
			char c[32];

			snprintf(c, sizeof(c), "volume %.2f", f);
			handle_cmd(&v, c, r, sizeof(r));
			if (vol_want < last)
				mono = 0;
			last = vol_want;
		}
		EXPECT(mono && last == 5);
	}
	reset();
	const char *bad[] = { "volume 1.5", "volume -0.1", "volume nan", "volume abc", "volume 0.3x", "volume " };
	for (i = 0; i < 6; i++) {
		handle_cmd(&v, bad[i], r, sizeof(r));
		EXPECT(!strncmp(r, "ERR 22", 6));
	}
	EXPECT(w_n == 0 && vol_want == 5);
	/* DSP rejects the step: error reported, requested level not recorded as applied */
	fail_name = "VoiceMMode1 RX Volume Step";
	fail_err = EIO;
	handle_cmd(&v, "volume 0.4", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 5 ", 6) && vol_want == 5);
	/* kernel without the rx-volume patch */
	fail_err = ENOENT;
	handle_cmd(&v, "volume 0.4", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 2 ", 6));
	handle_cmd(&v, "getvolume", r, sizeof(r));
	EXPECT(!strcmp(r, "OK volume=5"));
	/* re-asserted before every PCM open */
	reset();
	vol_want = 2;
	voice_open(&v);
	for (found = 0, i = 0; i < w_n; i++)
		if (!strcmp(w_name[i], "VoiceMMode1 RX Volume Step") && w_val[i] == 2)
			found = 1;
	EXPECT(found);
	/* -V: other table sizes */
	vol_max_step = 7;
	handle_cmd(&v, "volume 1", r, sizeof(r));
	EXPECT(!strcmp(r, "OK volume=7"));
	vol_max_step = 5;
	vol_want = -1;
	reset();
}

static void test_routes(void)
{
	EXPECT(rx_route_for("speaker") == RX_SPK);
	EXPECT(rx_route_for("earpiece") == RX_EAR);
	EXPECT(rx_route_for("headset") == RX_EAR);
	EXPECT(rx_route_for("") == RX_EAR);
	EXPECT(rx_route_for(NULL) == RX_EAR);
	reset();
	EXPECT(routes(0, 1, RX_SPK) == 0);
	/* the earpiece port is cleared before the speaker port is set (last set wins in q6voice) */
	EXPECT(w_n == 3);
	EXPECT(!strcmp(w_name[0], "LPI_MI2S_RX_0 Voice Mixer VoiceMMode1") && w_val[0] == 0);
	EXPECT(!strcmp(w_name[1], "TERT_MI2S_RX Voice Mixer VoiceMMode1") && w_val[1] == 1);
	EXPECT(!strcmp(w_name[2], "VoiceMMode1 Capture Mixer LPI_MI2S_TX_3") && w_val[2] == 1);
	reset();
	EXPECT(routes(0, 1, RX_EAR) == 0);
	EXPECT(!strcmp(w_name[0], "TERT_MI2S_RX Voice Mixer VoiceMMode1") && w_val[0] == 0);
	EXPECT(!strcmp(w_name[1], "LPI_MI2S_RX_0 Voice Mixer VoiceMMode1") && w_val[1] == 1);
	/* DT without the TFA speaker: clearing the missing TERT control is not an error for the earpiece route */
	reset();
	fail_name = "TERT_MI2S_RX Voice Mixer VoiceMMode1";
	fail_err = ENOENT;
	EXPECT(routes(0, 1, RX_EAR) == 0);
	EXPECT(routes(0, 1, RX_SPK) != 0);	/* but speakerphone cannot be set */
	reset();
	EXPECT(routes(0, 0, RX_SPK) == 0 && w_n == 3);
}

static void test_fail_closed(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };

	reset();
	mute_want = 1;
	fail_name = "VoiceMMode1 TX Mute";
	fail_err = EIO;
	EXPECT(voice_open(&v) == -1);
	EXPECT(v.tx == -1 && v.rx == -1);
	/* routes set then cleared again, no PCM opened */
	EXPECT(w_n >= 3 && !strcmp(w_name[w_n - 1], "VoiceMMode1 Capture Mixer LPI_MI2S_TX_3") && w_val[w_n - 1] == 0);
	mute_want = -1;
}

static const char *sock_path = "/tmp/a6l-q6voiced-test.sock";

static void *client(void *arg)
{
	char *out = arg;
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	ssize_t n;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && write(fd, "mute 1\n", 7) == 7) {
		n = read(fd, out, 63);
		out[n > 0 ? n : 0] = '\0';
	}
	close(fd);
	return NULL;
}

static void test_socket(void)
{
	struct voice v = { .card = 0, .dev = -1, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_SPK };
	char out[64] = "";
	pthread_t t;
	int lfd;

	reset();
	mute_want = -1;
	unsetenv("ANDROID_SOCKET_a6l_q6voiced");
	lfd = ctl_socket_open(sock_path);
	EXPECT(lfd >= 0);
	pthread_create(&t, NULL, client, out);
	serve_one(lfd, &v);
	pthread_join(t, NULL);
	EXPECT(!strcmp(out, "OK mute=1\n"));
	EXPECT(mute_want == 1);
	close(lfd);
	unlink(sock_path);
	mute_want = -1;
}

static void test_prop_str(void)
{
	const char *p = "/tmp/a6l-q6voiced-test.prop";
	char b[92];
	FILE *f = fopen(p, "w");

	fputs("speaker\n", f);
	fclose(f);
	prop_str(p, b, sizeof(b));
	EXPECT(!strcmp(b, "speaker") && rx_route_for(b) == RX_SPK);
	unlink(p);
	prop_str(p, b, sizeof(b));
	EXPECT(b[0] == '\0' && rx_route_for(b) == RX_EAR);
}

/* ---- r5 pass2 F19: fake PCM open (fails fo_fail times, then succeeds with /dev/null fds) */
static int fo_fail, fo_calls;
static long long fo_t[256];
static int fo_route[256];
static int fake_open(struct voice *v)
{
	if (fo_calls < 256) {
		fo_route[fo_calls] = v->rx_route;
		fo_t[fo_calls] = 0;
	}
	fo_calls++;
	if (fo_fail > 0) {
		fo_fail--;
		return -1;
	}
	v->tx = open("/dev/null", O_RDONLY | O_CLOEXEC);
	v->rx = open("/dev/null", O_RDONLY | O_CLOEXEC);
	return 0;
}

/* run the daemon step every 250 ms (the daemon loop rate) from t0 for ms milliseconds */
static void run_loop(struct voice *v, struct retry *rt, int want, int rx, long long *t, long long ms)
{
	long long end = *t + ms;

	for (; *t < end; *t += 250) {
		int before = fo_calls;

		daemon_step(v, rt, want, rx, *t);
		if (fo_calls != before && before < 256)
			fo_t[before] = *t;
	}
}

static void test_retry_whole_call(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	struct retry rt = { 0, 0 };
	long long t = 0, maxgap = 0;
	int i;

	reset();
	voice_open_fn = fake_open;
	fo_calls = 0;
	fo_fail = 20;		/* DSP not ready for a long time (> the old 5-attempt limit) */
	run_loop(&v, &rt, 1, RX_EAR, &t, 20000);
	EXPECT(fo_calls > 5);			/* the old daemon stopped at exactly 5 */
	EXPECT(v.tx < 0 && rt.fails == fo_calls);
	EXPECT(fo_t[4] <= 1000);		/* first 5 attempts at the loop rate, as before */
	for (i = 1; i < fo_calls && i < 256; i++)
		if (fo_t[i] - fo_t[i - 1] > maxgap)
			maxgap = fo_t[i] - fo_t[i - 1];
	EXPECT(maxgap <= RETRY_MAX_MS);	/* capped backoff */
	EXPECT(fo_calls <= 5 + 20000 / 500);	/* but rate limited */
	/* PCM/DSP becomes ready later in the SAME call: recovered without redial within the cap */
	fo_fail = 0;
	run_loop(&v, &rt, 1, RX_EAR, &t, RETRY_MAX_MS + 250);
	EXPECT(v.tx >= 0 && v.rx >= 0 && rt.fails == 0);
	/* hang-up closes, then the next call opens at once */
	run_loop(&v, &rt, 0, RX_EAR, &t, 250);
	EXPECT(v.tx < 0);
	i = fo_calls;
	run_loop(&v, &rt, 1, RX_EAR, &t, 250);
	EXPECT(fo_calls == i + 1 && v.tx >= 0);
	run_loop(&v, &rt, 0, RX_EAR, &t, 250);
	voice_open_fn = voice_open;
}

static void test_retry_hangup_and_route(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	struct retry rt = { 0, 0 };
	long long t = 0;
	int n;

	reset();
	voice_open_fn = fake_open;
	fo_calls = 0;
	fo_fail = 7;
	run_loop(&v, &rt, 1, RX_EAR, &t, 2000);	/* in backoff now */
	EXPECT(rt.fails >= 6 && rt.next_ms > t);
	/* hang-up during the backoff: cancelled, no more attempts, the next call starts fresh */
	n = fo_calls;
	run_loop(&v, &rt, 0, RX_EAR, &t, 10000);
	EXPECT(fo_calls == n && rt.fails == 0 && rt.next_ms == 0 && v.tx < 0);
	fo_fail = 1000;
	run_loop(&v, &rt, 1, RX_EAR, &t, 3000);
	EXPECT(rt.fails >= 6 && rt.next_ms > t);	/* in backoff again */
	/* speakerphone pressed during the backoff: immediate attempt on the new port */
	n = fo_calls;
	fo_fail = 0;
	run_loop(&v, &rt, 1, RX_SPK, &t, 250);
	EXPECT(fo_calls == n + 1 && fo_route[n] == RX_SPK && v.tx >= 0 && v.rx_route == RX_SPK);
	/* F7 kept: route change on an open path closes and reopens it on the new port */
	n = fo_calls;
	run_loop(&v, &rt, 1, RX_EAR, &t, 250);
	EXPECT(fo_calls == n + 1 && fo_route[n] == RX_EAR && v.tx >= 0 && v.rx_route == RX_EAR);
	run_loop(&v, &rt, 0, RX_EAR, &t, 250);
	EXPECT(v.tx < 0);
	/* the fail-closed mute still blocks the real open (no fake): retried, never opened */
	voice_open_fn = voice_open;
	mute_want = 1;
	fail_name = "VoiceMMode1 TX Mute";
	fail_err = EIO;
	rt.fails = 0;
	rt.next_ms = 0;
	run_loop(&v, &rt, 1, RX_EAR, &t, 3000);
	EXPECT(v.tx < 0 && rt.fails > 5);
	run_loop(&v, &rt, 0, RX_EAR, &t, 250);
	mute_want = -1;
	reset();
}

/* ---- r5 bug hunt round2 A1: card disconnected under the open voice PCMs (ADSP SSR during a call) */
static int alive_ok = 1, alive_calls;
static int fake_alive(int fd) { (void)fd; alive_calls++; return alive_ok; }

static void test_card_loss(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR, .dev_auto = 1 };
	struct retry rt = { 0, 0 };
	long long t = 0;
	int n;

	reset();
	voice_open_fn = fake_open;
	pcm_alive = fake_alive;
	alive_ok = 1;
	alive_calls = 0;
	fo_calls = 0;
	fo_fail = 0;
	run_loop(&v, &rt, 1, RX_EAR, &t, 1000);
	EXPECT(v.tx >= 0 && fo_calls == 1 && alive_calls > 0);	/* healthy path: probed, never reopened */
	/* ADSP restart: every ioctl on the old fds -> ENODEV; the kernel waits for us to close them */
	alive_ok = 0;
	fo_fail = 1000;		/* card not back yet */
	run_loop(&v, &rt, 1, RX_EAR, &t, 250);
	EXPECT(v.tx < 0 && v.rx < 0);		/* released at the first probe (the old daemon kept them all call long) */
	EXPECT(v.dev == -1);			/* looked up again (the new card may renumber the PCM) */
	run_loop(&v, &rt, 1, RX_EAR, &t, 8000);
	EXPECT(v.tx < 0 && rt.fails > 5 && fo_calls <= 1 + 5 + 8000 / 500);	/* F19 capped retry while it is gone */
	/* card back in the same call: reopened within the backoff cap, without redial */
	alive_ok = 1;
	fo_fail = 0;
	n = fo_calls;
	run_loop(&v, &rt, 1, RX_EAR, &t, RETRY_MAX_MS + 250);
	EXPECT(v.tx >= 0 && fo_calls == n + 1 && rt.fails == 0);
	run_loop(&v, &rt, 0, RX_EAR, &t, 250);
	EXPECT(v.tx < 0);
	/* explicit -d: the device number is kept */
	v.dev = 7;
	v.dev_auto = 0;
	run_loop(&v, &rt, 1, RX_EAR, &t, 250);
	alive_ok = 0;
	fo_fail = 1;
	run_loop(&v, &rt, 1, RX_EAR, &t, 250);
	EXPECT(v.tx < 0 && v.dev == 7);
	run_loop(&v, &rt, 0, RX_EAR, &t, 250);
	alive_ok = 1;
	pcm_alive = pcm_alive_real;
	voice_open_fn = voice_open;
	/* the real probe: a live fd is alive, only ENODEV means disconnected */
	n = open("/dev/null", O_RDONLY | O_CLOEXEC);
	EXPECT(pcm_alive_real(n) == 1);	/* ENOTTY: not a disconnected card */
	close(n);
}

/* ---- r5 bug hunt round2 A2: requests before the card exists (daemon started with class hal) */
static void test_no_card(void)
{
	struct voice v = { .card = 0, .dev = -1, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	char r[128];

	reset();
	mute_want = -1;
	vol_want = -1;
	fail_name = "VoiceMMode1 TX Mute";
	fail_err = ENODEV;
	handle_cmd(&v, "mute 0", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=0") && mute_want == 0);	/* was "ERR 19" */
	handle_cmd(&v, "mute 1", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 19 ", 7) && mute_want == 0);	/* a mute is never stored unapplied */
	mute_want = 1;						/* (as if accepted earlier with a card) */
	fail_name = "VoiceMMode1 RX Volume Step";
	handle_cmd(&v, "volume 0.8", r, sizeof(r));
	EXPECT(!strcmp(r, "OK volume=4") && vol_want == 4);
	/* other errors are still reported */
	fail_err = EIO;
	handle_cmd(&v, "volume 0.2", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 5 ", 6) && vol_want == 4);
	/* no PCM device known and no lookup allowed: open fails before touching any control */
	reset();
	EXPECT(voice_open(&v) == -1 && w_n == 0);
	/* card present: both stored values are asserted before the PCM open (mute fail closed) */
	v.dev = 99;
	EXPECT(voice_open(&v) == -1);	/* no /dev/snd on the host */
	{
		int i, m = 0, vol = 0;

		for (i = 0; i < w_n; i++) {
			if (!strcmp(w_name[i], "VoiceMMode1 TX Mute") && w_val[i] == 1)
				m = 1;
			if (!strcmp(w_name[i], "VoiceMMode1 RX Volume Step") && w_val[i] == 4)
				vol = 1;
		}
		EXPECT(m && vol);
	}
	/* an accepted unmute is re-asserted as well (a stale kernel mute must not survive) */
	reset();
	mute_want = 0;
	EXPECT(voice_open(&v) == -1);
	{
		int i, u = 0;

		for (i = 0; i < w_n; i++)
			if (!strcmp(w_name[i], "VoiceMMode1 TX Mute") && w_val[i] == 0)
				u = 1;
		EXPECT(u);
	}
	/* a failing unmute re-assert does not block the call (it cannot open a live microphone) */
	reset();
	fail_name = "VoiceMMode1 TX Mute";
	fail_err = EIO;
	fo_calls = 0;
	EXPECT(voice_open(&v) == -1);	/* fails only at the PCM open (host), not at the mute */
	EXPECT(w_n >= 1 && !strcmp(w_name[w_n - 1], "VoiceMMode1 Capture Mixer LPI_MI2S_TX_3") && w_val[w_n - 1] == 0);
	mute_want = -1;
	vol_want = -1;
	reset();
}

/* ---- btcall (29 Sep 2026): AudioFlinger software bridge of the call to a BT headset (default off) */
static int w_find(const char *name, long val)
{
	int i;

	for (i = 0; i < w_n; i++)
		if (!strcmp(w_name[i], name) && w_val[i] == val)
			return 1;
	return 0;
}

static int fbusy, fbusy_calls, fbusy_dev;
static int fake_busy(int card, int dev, int cap) { (void)card; (void)cap; fbusy_calls++; fbusy_dev = dev; return fbusy; }

static void bt_reset(void)
{
	memset(&bt, 0, sizeof(bt));
	bt.cvp_mute = 1;
	bt.dl_pcm = 3;
	pcm_busy = fake_busy;
	fbusy = 0;
	bt.dl_set = bt.ul_set = -1;
	snprintf(bt.dl_ctl, sizeof(bt.dl_ctl), "%s", BT_DL_CTL_DEF);
	snprintf(bt.ul_ctl, sizeof(bt.ul_ctl), "%s", BT_UL_CTL_DEF);
	mute_want = -1;
	reset();
}

static void test_btcall_disabled(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	char r[128];

	bt_reset();
	fbusy = 1, bt_step(&v, 1, 1000);		/* bridge property off: nothing touched, never active */
	EXPECT(w_n == 0 && !bt.active);
	handle_cmd(&v, "status", r, sizeof(r));
	EXPECT(!strcmp(r, "OK open=0 rx=earpiece mute=0"));	/* unchanged reply without the bridge */
	handle_cmd(&v, "mute 0", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=0") && w_n == 1 && w_find("VoiceMMode1 TX Mute", 0));
	EXPECT(rx_route_for("bridge") == RX_EAR);	/* the DSP keeps the earpiece port as its clock */
	bt_reset();
}

static void test_btcall_routes(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	char r[128];

	bt_reset();
	bt.enabled = 1;
	fbusy = 0, bt_step(&v, 0, 1000);		/* enabled, no call: both ADM routes connected ahead of the call */
	EXPECT(w_n == 2 && w_find("MultiMedia3 Mixer INCALL_RECORD_RX", 1) &&
	       w_find("VOICE_PLAYBACK_TX Audio Mixer MultiMedia4", 1) && !bt.active);
	reset();
	fbusy = 0, bt_step(&v, 0, 1250);
	EXPECT(w_n == 0);			/* idempotent */
	/* Android mute outside a bridged call: DSP mute + uplink injection disconnected; unmute reconnects it */
	handle_cmd(&v, "mute 1", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=1") && w_find("VoiceMMode1 TX Mute", 1) &&
	       w_find("VOICE_PLAYBACK_TX Audio Mixer MultiMedia4", 0) && !w_find("MultiMedia3 Mixer INCALL_RECORD_RX", 0));
	reset();
	handle_cmd(&v, "mute 0", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=0") && w_find("VoiceMMode1 TX Mute", 0) &&
	       w_find("VOICE_PLAYBACK_TX Audio Mixer MultiMedia4", 1));
	/* control names changed by property: the old routes are cleared, the new ones set */
	reset();
	snprintf(bt.dl_ctl, sizeof(bt.dl_ctl), "MultiMedia5 Mixer INCALL_RECORD_RX");
	fbusy = 0, bt_step(&v, 0, 1500);
	EXPECT(w_n == 2 && w_find("MultiMedia3 Mixer INCALL_RECORD_RX", 0) && w_find("MultiMedia5 Mixer INCALL_RECORD_RX", 1));
	/* disabled again: both cleared once, then nothing */
	reset();
	bt.enabled = 0;
	fbusy = 0, bt_step(&v, 0, 1750);
	EXPECT(w_n == 2 && w_find("MultiMedia5 Mixer INCALL_RECORD_RX", 0) &&
	       w_find("VOICE_PLAYBACK_TX Audio Mixer MultiMedia4", 0));
	reset();
	fbusy = 0, bt_step(&v, 0, 2000);
	EXPECT(w_n == 0);
	bt_reset();
}

static void test_btcall_missing_ctl(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	int i;

	bt_reset();
	bt.enabled = 1;
	fail_name = "MultiMedia3 Mixer INCALL_RECORD_RX";	/* kernel without the in-call record port */
	fail_err = ENOENT;
	fbusy = 0, bt_step(&v, 0, 1000);
	EXPECT(bt.fails == 1 && bt.retry_ms == 6000 && bt.dl_set == -1 && bt.ul_set == 1);
	w_n = 0;
	for (i = 1; i < 20; i++)
		fbusy = 0, bt_step(&v, 0, 1000 + i * 250);	/* no write storm inside the backoff */
	EXPECT(w_n == 0 && bt.fails == 1);
	fbusy = 0, bt_step(&v, 0, 6000);
	fbusy = 0, bt_step(&v, 0, 11000);
	EXPECT(bt.fails == 3 && bt.retry_ms == 71000);	/* then once a minute */
	/* the kernel gains the control (module reload): recovered at the next attempt */
	fail_name = NULL;
	fbusy = 0, bt_step(&v, 0, 71000);
	EXPECT(bt.fails == 0 && bt.dl_set == 1);
	bt_reset();
}

static void test_btcall_call(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };
	char r[128];

	bt_reset();
	bt.enabled = 1;
	fbusy = 0, bt_step(&v, 1, 1000);	/* call on the earpiece: not bridged */
	EXPECT(!bt.active);
	/* call open, Android moves it to the headset: phone mic muted live in the DSP */
	v.tx = open("/dev/null", O_RDONLY | O_CLOEXEC);
	v.rx = open("/dev/null", O_RDONLY | O_CLOEXEC);
	reset();
	fbusy_calls = 0;
	fbusy = 1, bt_step(&v, 1, 1250);
	EXPECT(bt.active && w_n == 1 && w_find("VoiceMMode1 TX Mute", 1));
	EXPECT(fbusy_calls == 1 && fbusy_dev == 3);	/* the downlink front end <prefix>dl_pcm is probed */
	handle_cmd(&v, "status", r, sizeof(r));
	EXPECT(!strcmp(r, "OK open=1 rx=earpiece mute=0 bridge=1"));
	/* Android unmute while bridged: the phone mic stays muted, the injected headset mic stays connected */
	reset();
	handle_cmd(&v, "mute 0", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=0") && w_find("VoiceMMode1 TX Mute", 1) && !w_find("VoiceMMode1 TX Mute", 0));
	/* Android mute while bridged: uplink injection disconnected */
	reset();
	handle_cmd(&v, "mute 1", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=1") && w_find("VOICE_PLAYBACK_TX Audio Mixer MultiMedia4", 0));
	/* ... and the uplink route cannot be cleared: the mute is reported as failed */
	reset();
	handle_cmd(&v, "mute 0", r, sizeof(r));
	fail_name = "VOICE_PLAYBACK_TX Audio Mixer MultiMedia4";
	fail_err = EIO;
	handle_cmd(&v, "mute 1", r, sizeof(r));
	EXPECT(!strncmp(r, "ERR 5 btcall uplink", 19));
	fail_name = NULL;
	handle_cmd(&v, "mute 0", r, sizeof(r));
	EXPECT(!strcmp(r, "OK mute=0"));
	/* back to the earpiece mid-call: the phone mic is unmuted again (Android's mute is 0) */
	reset();
	fbusy = 0, bt_step(&v, 1, 1500);
	EXPECT(!bt.active && w_n == 1 && w_find("VoiceMMode1 TX Mute", 0));
	/* Android's mute stays when the bridge ends */
	fbusy = 1, bt_step(&v, 1, 1750);
	handle_cmd(&v, "mute 1", r, sizeof(r));
	reset();
	fbusy = 0, bt_step(&v, 1, 2000);
	EXPECT(w_n == 0 && !bt.active);
	close(v.tx);
	close(v.rx);
	v.tx = v.rx = -1;
	/* cvp_mute=0: bridged but the phone mic is left to the codec path (off by a6l-audio-route) */
	mute_want = 0;
	bt.cvp_mute = 0;
	reset();
	fbusy = 1, bt_step(&v, 1, 2250);
	EXPECT(bt.active && !bt_mic_mute());
	/* no valid dl_pcm: never bridged, /proc not read */
	bt.dl_pcm = -1;
	bt.active = 0;
	fbusy_calls = 0;
	fbusy = 1, bt_step(&v, 1, 2300);
	EXPECT(!bt.active && fbusy_calls == 0);
	bt.dl_pcm = 3;
	fbusy = 1, bt_step(&v, 1, 2350);
	EXPECT(bt.active);
	/* hang-up clears the bridge */
	fbusy = 1, bt_step(&v, 0, 2500);
	EXPECT(!bt.active);
	bt_reset();
}

static void test_btcall_open(void)
{
	struct voice v = { .card = 0, .dev = 99, .tx = -1, .rx = -1, .set_routes = 1, .rx_route = RX_EAR };

	/* call starts already bridged: mic mute + routes asserted before the PCM open, no stale unmute written */
	bt_reset();
	bt.enabled = 1;
	fbusy = 1, bt_step(&v, 1, 1000);
	EXPECT(bt.active);
	mute_want = 0;
	reset();
	EXPECT(voice_open(&v) == -1);		/* host: no PCM */
	EXPECT(w_find("VoiceMMode1 TX Mute", 1) && !w_find("VoiceMMode1 TX Mute", 0));
	/* routes already in place: not rewritten */
	EXPECT(!w_find("MultiMedia3 Mixer INCALL_RECORD_RX", 1));
	/* after a card loss the routes are unknown and rewritten at the next open */
	bt.dl_set = bt.ul_set = -1;
	reset();
	voice_open(&v);
	EXPECT(w_find("MultiMedia3 Mixer INCALL_RECORD_RX", 1) && w_find("VOICE_PLAYBACK_TX Audio Mixer MultiMedia4", 1));
	/* a failing bridge mic mute does not block the call (not Android's mute) */
	reset();
	fail_name = "VoiceMMode1 TX Mute";
	fail_err = EIO;
	EXPECT(voice_open(&v) == -1);
	EXPECT(w_n >= 1 && !strcmp(w_name[w_n - 1], "VoiceMMode1 Capture Mixer LPI_MI2S_TX_3"));	/* reached the PCM open */
	bt_reset();
}

static void test_btcall_cfg(void)
{
	const char *pre = "/tmp/a6l-q6voiced-test.btcall.";
	const char *k[] = { "bridge", "cvp_mute", "dl_ctl", "ul_ctl", "dl_pcm" };
	const char *val[] = { "1\n", "0\n", "MultiMedia6 Mixer INCALL_RECORD_RX\n", "\n", "5\n" };
	char f[128];
	int i;

	for (i = 0; i < 5; i++) {
		FILE *fp;

		snprintf(f, sizeof(f), "%s%s", pre, k[i]);
		fp = fopen(f, "w");
		fputs(val[i], fp);
		fclose(fp);
	}
	bt_prefix = pre;
	bt_load_cfg();
	EXPECT(bt.enabled == 1 && bt.cvp_mute == 0 && !strcmp(bt.dl_ctl, "MultiMedia6 Mixer INCALL_RECORD_RX") &&
	       !strcmp(bt.ul_ctl, BT_UL_CTL_DEF) && bt.dl_pcm == 5);
	/* too long for an ALSA control name: default kept */
	snprintf(f, sizeof(f), "%sul_ctl", pre);
	{
		FILE *fp = fopen(f, "w");

		fputs("0123456789012345678901234567890123456789012345\n", fp);
		fclose(fp);
	}
	bt_load_cfg();
	EXPECT(!strcmp(bt.ul_ctl, BT_UL_CTL_DEF));
	snprintf(f, sizeof(f), "%sdl_pcm", pre);
	{
		FILE *fp = fopen(f, "w");

		fputs("0\n", fp);	/* MultiMedia1 = media playback: refused */
		fclose(fp);
	}
	bt_load_cfg();
	EXPECT(bt.dl_pcm == -1);
	for (i = 0; i < 5; i++) {
		snprintf(f, sizeof(f), "%s%s", pre, k[i]);
		unlink(f);
	}
	bt_load_cfg();			/* nothing set = the product default: off, CVP mute on */
	EXPECT(bt.enabled == 0 && bt.cvp_mute == 1 && !strcmp(bt.dl_ctl, BT_DL_CTL_DEF) && bt.dl_pcm == -1);
	/* the real /proc probe: a missing status file is "not open" */
	EXPECT(pcm_busy_real(97, 3, 1) == 0);
	bt_prefix = "persist.vendor.a6l.btcall.";
	bt_reset();
}

int main(void)
{
	ctl_write = fake_write;
	setvbuf(stdout, NULL, _IONBF, 0);
	test_mute_cmd();
	test_volume_cmd();
	test_routes();
	test_fail_closed();
	test_socket();
	test_prop_str();
	test_retry_whole_call();
	test_retry_hangup_and_route();
	test_card_loss();
	test_no_card();
	test_btcall_disabled();
	test_btcall_routes();
	test_btcall_missing_ctl();
	test_btcall_call();
	test_btcall_open();
	test_btcall_cfg();
	printf("A6L_Q6VOICED_TESTS %s pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
