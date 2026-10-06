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
 *   -s <session> (volte3, 26 Sep 2026): before every open, write q6mvm's mmode1_session parameter
 *       (/sys/module/q6mvm/parameters/mmode1_session): default|cs|volte|mmode1|mmode2|<8 hex VSID>. It selects
 *       the ADSP MVM passive session the VoiceMMode1 PCMs join: "11C05000" (default; CS call proven 24 Sep),
 *       "default volte voice" (volte) if the modem's LTE voice VSID is 0x10C02000 (a6l-qmi ims-status).
 *       Needs root (attended tests); in the ROM init writes it (a6l-q6voiced.rc, vendor.a6l.voice.domain).
 *   -d default: the pcm named "VoiceMMode1" in /proc/asound/pcm; -c default 0.
 *   -r: also set the DSP routes (LPI_MI2S_RX_0 Voice Mixer VoiceMMode1, VoiceMMode1 Capture Mixer LPI_MI2S_TX_3)
 *       on open and clear them on close. Analog (codec) routing stays with a6l-audio-route / mixer_paths.
 * r5 review fixes (28 Sep 2026):
 *   F6 call mute: the daemon owns the uplink mute of the voice path. Control socket (init: socket a6l_q6voiced, or
 *       -S <path> for host tests), one request line per connection:
 *         "mute 0|1" -> writes the ALSA control "VoiceMMode1 TX Mute" (kernel patch a6l-q6voice-tx-mute-v75: CVP
 *                       VSS_IVOLUME_CMD_MUTE_V2, applied live during a call, kept and restored at every call start)
 *                       reply "OK mute=N" or "ERR <errno> <text>" (ENOENT = kernel without the control)
 *         "getmute"  -> "OK mute=N";  "status" -> "OK open=0|1 rx=earpiece|speaker mute=N"
 *       Both the radio HAL (IRadioVoice.setMute) and the audio HAL (IModule.setMicMute, tree patch
 *       audio/patches/0002) use it, so Android has one applied mute state. Re-applied before every PCM open
 *       (fail closed: a requested mute that cannot be applied does not start the voice path).
 *   F7 speakerphone: the DSP voice RX port follows vendor.a6l.audio.call_out (published by the audio HAL from the
 *       Telephony Rx patch): "speaker" -> TERT_MI2S_RX Voice Mixer VoiceMMode1 (TFA9894), anything else ->
 *       LPI_MI2S_RX_0 (codec earpiece/headset). The CVP session takes its ports at creation, so a change during a
 *       call closes and reopens the voice PCMs (short gap; the mute state is restored by the kernel).
 *       -o <prop> overrides the property (a file path on host builds).
 * r5 review pass2 F19 (28 Sep 2026): a failed open is retried for as long as the call stays active (it used to stop
 *       after 5 attempts, leaving the rest of the call silent). The first 5 retries keep the 250 ms loop rate, then
 *       a capped backoff (0.5, 1, 2, 4, 4 ... s); hang-up cancels it at once, an RX route change retries at once.
 * r5 review round4 F37 (28 Sep 2026): in-call (downlink) volume. Socket request "volume <0..1>" (Android's voice
 *       volume, from the audio HAL Telephony voiceVolume, tree patch audio/patches/0003) -> step round(v * max),
 *       max = -V (default 5 = stock msm HAL MAX_VOL_INDEX; 0 is the lowest calibrated level, not mute) -> ALSA
 *       control "VoiceMMode1 RX Volume Step" (kernel patch a6l-q6voice-rx-volume-v75: CVP VSS_IVOLUME_CMD_SET_STEP,
 *       RX, applied live, kept and restored at every call start). Reply "OK volume=<step>" or "ERR <errno> <text>";
 *       the step is only remembered when the kernel accepted it. "getvolume" -> "OK volume=<step>" (-1 = never set).
 *       Re-asserted before every PCM open (not fatal: the call starts at the DSP default level).
 * r5 bug hunt round2 (29 Sep 2026):
 *   A1 card loss during a call (ADSP SSR / audio PD restart / card unbind): the kernel replaces the file operations of
 *       our open PCMs with "disconnected" ones (every ioctl -ENODEV) and ASoC's soc_cleanup_card_resources() waits in
 *       snd_card_disconnect_sync() until every file of the old card is closed. The daemon used to keep the dead fds
 *       for the rest of the call: silent call and the DSP/card recovery blocked until hang-up. The open pair is now
 *       probed every loop (SNDRV_PCM_IOCTL_PVERSION: -ENODEV = card gone); on loss it is closed at once and reopened
 *       through the F19 retry (the VoiceMMode1 device is looked up again when it was not given with -d).
 *   A2 the daemon no longer needs the card to run: it is started with class hal (before system_server, so Android's
 *       boot-time setVoiceVolume/setMicMute reach it; the audio policy caches the voice volume and does not resend an
 *       unchanged value) and looks up the VoiceMMode1 PCM when a call starts. Without a card (-ENODEV) "mute"/"volume"
 *       "volume" and "mute 0" requests are stored and answered OK: no voice path can exist without the card and both
 *       are re-asserted before every PCM open (the unmute too, so a kernel state left from before cannot keep the
 *       uplink muted). "mute 1" without a card is still an error (stored, it would fail every later call closed on a
 *       kernel without the TX Mute control).
 * btcall (29 Sep 2026, docs/android-bt-audio-20260929.md): cellular call audio to a Bluetooth (HFP) headset through
 *       an AudioFlinger software bridge. OFF unless <prefix>bridge=1 (prefix -b, default persist.vendor.a6l.btcall.).
 *       The ADSP voice session stays as it is (VoiceMMode1 PCMs held, RX on the earpiece port as the clock, analog
 *       output off by a6l-audio-route); the CPU gets the downlink from the in-call record port and returns the headset
 *       mic through the in-call music port (kernel work, see the doc). This daemon:
 *         - connects the two ADM routes while the bridge is enabled (before any call, so the HAL's Telephony Rx/Tx
 *           streams never open a front end without a back end): <prefix>dl_ctl (default "MultiMedia3 Mixer
 *           INCALL_RECORD_RX") and <prefix>ul_ctl (default "VOICE_PLAYBACK_TX Audio Mixer MultiMedia4"). Retried
 *           with a backoff (5 s, then 60 s) when the kernel has no such control; cleared when the bridge is disabled.
 *         - a call is bridged while it is active and the audio HAL reads the downlink, i.e. the in-call record front
 *           end pcmC<card>D<prefix>dl_pcm c is open (/proc/asound status not "closed"; the HAL opens it for the
 *           Telephony Rx stream of the software bridge, tree patch audio/patches/0004). Then the phone mic is muted in
 *           the DSP ("VoiceMMode1 TX Mute", unless <prefix>cvp_mute=0; the uplink is the injected headset mic only)
 *           and the voice RX stays on the earpiece port whatever vendor.a6l.audio.call_out says (DSP clock; the codec
 *           is silenced by a6l-audio-route, which makes the same check).
 *         - Android's mic mute ("mute 1") in bridge mode also disconnects the uplink injection route (the CVP mute alone
 *           would not silence the injected stream); "mute 0" reconnects it and keeps the phone mic muted.
 *         - "status" appends " bridge=0|1" when the bridge is enabled.
 * Log lines start with A6L_Q6VOICED. Exit 0 = ok.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

/* Android init discards this daemon's stdout/stderr. Keep CLI output for
 * attended/recovery tools and mirror it to logcat in the ROM build, preserving
 * errno so reporting an ALSA failure cannot change the caller's error state.
 */
#ifdef A6L_Q6VOICED_LOGCAT
#include <android/log.h>
#endif
static int q6_log(int error, const char *format, ...)
	__attribute__((format(printf, 2, 3)));
static int q6_log(int error, const char *format, ...)
{
	const int saved_errno = errno;
	va_list args;
	va_start(args, format);
#ifdef A6L_Q6VOICED_LOGCAT
	va_list logged;
	va_copy(logged, args);
	__android_log_vprint(error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
			    "a6l-q6voiced", format, logged);
	va_end(logged);
#endif
	const int ret = vfprintf(error ? stderr : stdout, format, args);
	va_end(args);
	errno = saved_errno;
	return ret;
}
#define q6_info(...) q6_log(0, __VA_ARGS__)
#define q6_error(...) q6_log(1, __VA_ARGS__)

#define RATE 8000
#define PERIOD_FRAMES 1024	/* 2048 bytes: q6voice_dai_hardware period_bytes 2048..4096 */
#define PERIODS 2

static volatile sig_atomic_t stop_req;
static void on_sig(int s) { (void)s; stop_req = 1; }

static const char *ROUTE_RX = "LPI_MI2S_RX_0 Voice Mixer VoiceMMode1";
static const char *ROUTE_RX_SPK = "TERT_MI2S_RX Voice Mixer VoiceMMode1";	/* r5 F7: TFA9894 speakerphone */
static const char *CTL_TX_MUTE = "VoiceMMode1 TX Mute";			/* r5 F6: kernel tx-mute patch */
static const char *OUT_PROP = "vendor.a6l.audio.call_out";		/* r5 F7: set by the audio HAL */
enum { RX_EAR = 0, RX_SPK = 1 };
static int mute_want = -1;	/* r5 F6: last mute accepted by the kernel, -1 = never requested */
static const char *CTL_RX_VOL = "VoiceMMode1 RX Volume Step";		/* r5 round4 F37: kernel rx-volume patch */
static int vol_max_step = 5;	/* F37: -V; stock msm audio HAL MAX_VOL_INDEX */
static int vol_want = -1;	/* F37: last volume step accepted by the kernel, -1 = never requested */
static const char *ROUTE_TX = "VoiceMMode1 Capture Mixer LPI_MI2S_TX_3";
static const char *SESSION_PARAM = "/sys/module/q6mvm/parameters/mmode1_session";
static const char *session_opt;	/* volte3: -s */

/* btcall (29 Sep 2026): AudioFlinger software bridge of the call audio to a BT SCO headset (default off) */
#define BT_CTL_MAX 44		/* struct snd_ctl_elem_id name */
static const char *bt_prefix = "persist.vendor.a6l.btcall.";	/* -b; host tests: a file path prefix */
static const char *BT_DL_CTL_DEF = "MultiMedia3 Mixer INCALL_RECORD_RX";		/* downlink -> CPU capture FE */
static const char *BT_UL_CTL_DEF = "VOICE_PLAYBACK_TX Audio Mixer MultiMedia4";	/* CPU playback FE -> uplink */
struct btcall {
	int enabled;			/* <prefix>bridge = 1 */
	int cvp_mute;			/* <prefix>cvp_mute != 0 (default 1): phone mic muted while bridged */
	int dl_pcm;			/* <prefix>dl_pcm: downlink capture front end (1..31), -1 = unset */
	char dl_ctl[BT_CTL_MAX], ul_ctl[BT_CTL_MAX];
	int active;			/* the current call is bridged */
	int dl_set, ul_set;		/* value the kernel accepted, -1 = unknown / never written */
	char dl_applied[BT_CTL_MAX], ul_applied[BT_CTL_MAX];	/* control names those values went to */
	int fails;
	long long retry_ms;
};
static struct btcall bt = { .cvp_mute = 1, .dl_pcm = -1, .dl_set = -1, .ul_set = -1 };
static long long mono_ms(void);

static int set_session(const char *s)
{
	char back[64] = "";
	FILE *f;

	if (!s)
		return 0;
	f = fopen(SESSION_PARAM, "w");
	if (!f || fputs(s, f) < 0 || fclose(f) != 0) {
		q6_error( "A6L_Q6VOICED session '%s': cannot write %s: %s\n", s, SESSION_PARAM, strerror(errno));
		return -1;
	}
	f = fopen(SESSION_PARAM, "r");
	if (f) {
		if (!fgets(back, sizeof(back), f))
			back[0] = '\0';
		fclose(f);
		back[strcspn(back, "\n")] = '\0';
	}
	q6_info("A6L_Q6VOICED session '%s' (q6mvm.mmode1_session='%s')\n", s, back);
	return 0;
}

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

/* 0 or -errno (r5: the error is reported to socket clients) */
static int ctl_write_real(int card, const char *name, long v)
{
	char path[64];
	struct snd_ctl_elem_value ev;
	int fd, ret, err = 0;

	snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? -ENODEV : -errno;	/* no card: not "no such control" */
	memset(&ev, 0, sizeof(ev));
	ev.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strncpy((char *)ev.id.name, name, sizeof(ev.id.name) - 1);
	ev.value.integer.value[0] = v;
	ret = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
	if (ret < 0)
		err = -errno;
	close(fd);
	return err;
}

static int (*ctl_write)(int card, const char *name, long v) = ctl_write_real;

static int ctl_set(int card, const char *name, long v)
{
	int ret = ctl_write(card, name, v);

	if (ret < 0)
		q6_error( "A6L_Q6VOICED ctl '%s' = %ld: %s\n", name, v, strerror(-ret));
	else
		q6_info("A6L_Q6VOICED ctl '%s' = %ld\n", name, v);
	return ret;
}

static const char *rx_ctl(int rx) { return rx == RX_SPK ? ROUTE_RX_SPK : ROUTE_RX; }
static const char *rx_name(int rx) { return rx == RX_SPK ? "speaker" : "earpiece"; }
/* r5 F7: Android's call output device (audio HAL property) -> DSP voice RX port */
static int rx_route_for(const char *call_out) { return (call_out && !strcmp(call_out, "speaker")) ? RX_SPK : RX_EAR; }

/* on: the other RX route is cleared FIRST (the q6voice path has one RX port, the last one set wins) */
static int routes(int card, int on, int rx)
{
	int r = 0;

	if (on) {
		ctl_set(card, rx_ctl(!rx), 0);	/* may not exist (DT without the TFA speaker): ignored */
		r |= ctl_set(card, rx_ctl(rx), 1) ? -1 : 0;
		r |= ctl_set(card, ROUTE_TX, 1) ? -1 : 0;
	} else {
		ctl_set(card, ROUTE_RX_SPK, 0);
		r |= ctl_set(card, ROUTE_RX, 0) ? -1 : 0;
		r |= ctl_set(card, ROUTE_TX, 0) ? -1 : 0;
	}
	return r;
}

/* btcall: 1 while the ALSA pcm is open (its /proc status is not "closed"), 0 otherwise */
static int pcm_busy_real(int card, int dev, int capture)
{
	char path[80], line[64] = "";
	FILE *f;

	snprintf(path, sizeof(path), "/proc/asound/card%d/pcm%d%c/sub0/status", card, dev, capture ? 'c' : 'p');
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (!fgets(line, sizeof(line), f))
		line[0] = '\0';
	fclose(f);
	return line[0] && strncmp(line, "closed", 6) != 0;
}
static int (*pcm_busy)(int card, int dev, int capture) = pcm_busy_real;	/* host tests: fake */

/* btcall: the phone mic is muted in the DSP while a call is bridged to the headset */
static int bt_mic_mute(void) { return bt.active && bt.cvp_mute; }

/* btcall: one ADM route toward `want` (0/1); a never-written route is not cleared (the control may not exist) */
static int bt_route_one(int card, const char *ctl, int want, int *set, char *applied)
{
	int e;

	if (*set == 1 && strcmp(applied, ctl)) {	/* control name changed (property edit): clear the old one */
		ctl_set(card, applied, 0);
		*set = -1;
	}
	if (*set == want || (!want && *set != 1))
		return 0;
	e = ctl_set(card, ctl, want);
	if (e < 0) {
		*set = -1;
		return e;
	}
	*set = want;
	snprintf(applied, BT_CTL_MAX, "%s", ctl);
	return 0;
}

/* btcall: DL route = bridge enabled; UL route = enabled and not muted. 0 or the first -errno */
static int bt_sync_routes(int card, long long now, int force)
{
	int r1, r2;

	if (!force && now < bt.retry_ms)
		return 0;
	r1 = bt_route_one(card, bt.dl_ctl, bt.enabled, &bt.dl_set, bt.dl_applied);
	r2 = bt_route_one(card, bt.ul_ctl, bt.enabled && mute_want != 1, &bt.ul_set, bt.ul_applied);
	if (r1 < 0 || r2 < 0) {
		bt.fails++;
		bt.retry_ms = now + (bt.fails < 3 ? 5000 : 60000);
		return r1 < 0 ? r1 : r2;
	}
	bt.fails = 0;
	bt.retry_ms = 0;
	return 0;
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
		q6_error( "A6L_Q6VOICED open %s: %s\n", path, strerror(errno));
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
		q6_error( "A6L_Q6VOICED %s HW_PARAMS: %s\n", what, strerror(errno));
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
		q6_error( "A6L_Q6VOICED %s SW_PARAMS: %s (ignored)\n", what, strerror(errno));
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		q6_error( "A6L_Q6VOICED %s PREPARE: %s\n", what, strerror(errno));
		goto err;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0)
		q6_error( "A6L_Q6VOICED %s START: %s (ignored, prepare is enough on older setups)\n", what, strerror(errno));
	q6_info("A6L_Q6VOICED %s %s open+prepared\n", what, path);
	return fd;
err:
	close(fd);
	return -1;
}

struct voice { int card, dev, tx, rx, set_routes, rx_route, dev_auto; };

/* r5 round2 A1: 0 when the PCM belongs to a disconnected card (snd_disconnect_ioctl returns -ENODEV for everything) */
static int pcm_alive_real(int fd)
{
	int ver = 0;

	return ioctl(fd, SNDRV_PCM_IOCTL_PVERSION, &ver) == 0 || errno != ENODEV;
}

static int (*pcm_alive)(int fd) = pcm_alive_real;	/* host tests: fake */

static int voice_open(struct voice *v)
{
	if (v->tx >= 0)
		return 0;
	/* r5 round2 A2: the card may appear (or come back after an ADSP restart) after the daemon started */
	if (v->dev < 0 && v->dev_auto)
		v->dev = find_voice_dev(v->card);
	if (v->dev < 0) {
		q6_info("A6L_Q6VOICED OPEN_FAIL no VoiceMMode1 pcm on card %d (/proc/asound/pcm)\n", v->card);
		fflush(stdout);
		return -1;
	}
	if (session_opt && set_session(session_opt))
		q6_error( "A6L_Q6VOICED warning: session not set (q6mvm without the volte3 patch?)\n");
	if (v->set_routes && routes(v->card, 1, v->rx_route))
		q6_error( "A6L_Q6VOICED warning: route controls not all set\n");
	/* btcall: re-assert the bridge ADM routes (card restarted since they were set) */
	if (bt.enabled && bt_sync_routes(v->card, mono_ms(), 1))
		q6_error( "A6L_Q6VOICED warning: btcall bridge routes not set (kernel without in-call record/music?)\n");
	/* r5 F6: re-assert the requested uplink mute (module reload / DSP recovery). Fail closed. */
	if (mute_want == 1 && ctl_set(v->card, CTL_TX_MUTE, 1) < 0) {
		q6_error( "A6L_Q6VOICED mute requested but not applicable: voice path NOT started\n");
		goto err;
	}
	/* btcall: bridged call: the phone mic is muted in the DSP (not fatal: the codec input path is off as well) */
	if (mute_want != 1 && bt_mic_mute() && ctl_set(v->card, CTL_TX_MUTE, 1) < 0)
		q6_error( "A6L_Q6VOICED warning: btcall phone mic not muted in the DSP\n");
	/* r5 round2 A2: an accepted unmute is re-asserted too (not fatal: never opens a live microphone by itself) */
	if (mute_want == 0 && !bt_mic_mute() && ctl_set(v->card, CTL_TX_MUTE, 0) < 0)
		q6_error( "A6L_Q6VOICED warning: unmute not re-applied (kernel keeps its state)\n");
	/* r5 round4 F37: re-assert the requested downlink volume (module reload); not fatal */
	if (vol_want >= 0 && ctl_set(v->card, CTL_RX_VOL, vol_want) < 0)
		q6_error( "A6L_Q6VOICED warning: volume step %d not applied (DSP default level)\n", vol_want);
	/* capture (tx) first, as q6voiced: q6voice starts the path when the second direction opens */
	v->tx = pcm_open_start(v->card, v->dev, 1);
	if (v->tx < 0)
		goto err;
	v->rx = pcm_open_start(v->card, v->dev, 0);
	if (v->rx < 0)
		goto err;
	q6_info("A6L_Q6VOICED OPEN card %d dev %d\n", v->card, v->dev);
	fflush(stdout);
	return 0;
err:
	if (v->tx >= 0)
		close(v->tx);
	v->tx = v->rx = -1;
	if (v->set_routes)
		routes(v->card, 0, v->rx_route);
	q6_info("A6L_Q6VOICED OPEN_FAIL (see kernel log: A6L_Q6VOICE / q6voice / q6cvp / q6mvm)\n");
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
		routes(v->card, 0, v->rx_route);
	q6_info("A6L_Q6VOICED CLOSED\n");
	fflush(stdout);
}

/* r5 pass2 F19: retry state of a failed open while the call is active */
#define RETRY_FAST 5		/* attempts at the daemon loop rate (as before F19) */
#define RETRY_MAX_MS 4000	/* backoff cap: DSP/PCM becoming ready later still recovers the call within 4 s */
struct retry { int fails; long long next_ms; };
static int (*voice_open_fn)(struct voice *v) = voice_open;	/* host tests: fake PCM open */

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long retry_delay_ms(int fails)
{
	long long d;

	if (fails < RETRY_FAST)
		return 0;
	d = 500LL << (fails - RETRY_FAST < 4 ? fails - RETRY_FAST : 4);
	return d > RETRY_MAX_MS ? RETRY_MAX_MS : d;
}

/* one daemon iteration: want = call active, rx = requested RX port, now = monotonic ms */
static void daemon_step(struct voice *v, struct retry *rt, int want, int rx, long long now)
{
	long long d;

	if (!want) {
		if (rt->fails)
			q6_info("A6L_Q6VOICED call ended: retry cancelled after %d failed opens\n", rt->fails);
		rt->fails = 0;
		rt->next_ms = 0;
		voice_close(v);
		return;
	}
	/* r5 F7: the CVP takes its RX port at creation: reopen on a route change during the call */
	if (v->tx >= 0 && v->set_routes && rx != v->rx_route) {
		q6_info("A6L_Q6VOICED rx route %s -> %s (reopen)\n", rx_name(v->rx_route), rx_name(rx));
		voice_close(v);
		rt->fails = 0;
		rt->next_ms = 0;
	}
	/* r5 round2 A1: card gone under the open PCMs (ADSP restart): release them now (the kernel waits for it) */
	if (v->tx >= 0 && (!pcm_alive(v->tx) || !pcm_alive(v->rx))) {
		q6_info("A6L_Q6VOICED voice PCMs lost (card disconnected): closing, reopening when the card is back\n");
		voice_close(v);
		if (bt.dl_set == 1)	/* btcall: the new card starts with the routes cleared */
			bt.dl_set = -1;
		if (bt.ul_set == 1)
			bt.ul_set = -1;
		if (v->dev_auto)
			v->dev = -1;
		rt->fails = 0;		/* F19 retry: 5 quick attempts, then the capped backoff until the card is back */
		rt->next_ms = now;
		return;
	}
	if (v->tx >= 0)
		return;
	if (rt->fails && rx != v->rx_route && now < rt->next_ms) {
		q6_info("A6L_Q6VOICED rx route %s -> %s during retry backoff: retry now\n", rx_name(v->rx_route), rx_name(rx));
		rt->next_ms = now;
	}
	if (now < rt->next_ms)
		return;
	v->rx_route = rx;
	if (!voice_open_fn(v)) {
		if (rt->fails)
			q6_info("A6L_Q6VOICED call audio recovered after %d failed opens\n", rt->fails);
		rt->fails = 0;
		rt->next_ms = 0;
		return;
	}
	rt->fails++;
	d = retry_delay_ms(rt->fails);
	rt->next_ms = now + d;
	if (d)
		q6_info("A6L_Q6VOICED open failed %d times, call still active: next retry in %lld ms\n", rt->fails, d);
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

static void prop_str(const char *prop, char *out, size_t n)
{
#ifdef __ANDROID__
	char val[PROP_VALUE_MAX] = "";
	__system_property_get(prop, val);
	snprintf(out, n, "%s", val);
#else
	FILE *f = fopen(prop, "r");
	out[0] = '\0';
	if (f) {
		if (!fgets(out, (int)n, f))
			out[0] = '\0';
		fclose(f);
	}
	out[strcspn(out, "\r\n")] = '\0';
#endif
}

/* btcall: a control name from a property; empty or too long for an ALSA control name -> the default */
static void bt_ctl_name(char dst[BT_CTL_MAX], const char *val, const char *def)
{
	const char *src = val[0] && strlen(val) < BT_CTL_MAX ? val : def;

	memcpy(dst, src, strlen(src) + 1);
}

/* btcall: configuration from <prefix>{bridge,cvp_mute,dl_ctl,ul_ctl} (read every loop: live changes) */
static void bt_load_cfg(void)
{
	char k[160], val[92];

	snprintf(k, sizeof(k), "%sbridge", bt_prefix);
	prop_str(k, val, sizeof(val));
	bt.enabled = val[0] == '1';
	snprintf(k, sizeof(k), "%scvp_mute", bt_prefix);
	prop_str(k, val, sizeof(val));
	bt.cvp_mute = val[0] != '0';
	snprintf(k, sizeof(k), "%sdl_pcm", bt_prefix);
	prop_str(k, val, sizeof(val));
	bt.dl_pcm = val[0] ? atoi(val) : -1;
	if (bt.dl_pcm < 1 || bt.dl_pcm > 31)	/* 0 = MultiMedia1 (media playback): never the bridge */
		bt.dl_pcm = -1;
	snprintf(k, sizeof(k), "%sdl_ctl", bt_prefix);
	prop_str(k, val, sizeof(val));
	bt_ctl_name(bt.dl_ctl, val, BT_DL_CTL_DEF);
	snprintf(k, sizeof(k), "%sul_ctl", bt_prefix);
	prop_str(k, val, sizeof(val));
	bt_ctl_name(bt.ul_ctl, val, BT_UL_CTL_DEF);
}

/* btcall: one daemon iteration (before daemon_step): routes + bridged-call state; want = call active */
static void bt_step(struct voice *v, int want, long long now)
{
	int act = bt.enabled && want && bt.dl_pcm > 0 && pcm_busy(v->card, bt.dl_pcm, 1);

	bt_sync_routes(v->card, now, 0);
	if (act == bt.active)
		return;
	q6_info("A6L_Q6VOICED btcall bridge %s\n", act ? "ON (call audio via AudioFlinger to the BT headset)" : "off");
	bt.active = act;
	/* live during an open call; otherwise voice_open applies it. Android's own mute stays as requested. */
	if (v->tx >= 0 && bt.cvp_mute && mute_want != 1 && ctl_set(v->card, CTL_TX_MUTE, act) < 0)
		q6_error( "A6L_Q6VOICED warning: btcall phone mic mute %d not applied\n", act);
}

/* r5 F6: one request line -> one reply line (no trailing newline in `reply`) */
static void handle_cmd(struct voice *v, const char *line, char *reply, size_t n)
{
	int m, r;

	if (!strncmp(line, "mute ", 5) && (line[5] == '0' || line[5] == '1') && line[6] == '\0') {
		m = line[5] - '0';
		/* btcall: a bridged call keeps the phone mic muted whatever Android asks */
		r = ctl_set(v->card, CTL_TX_MUTE, m || bt_mic_mute());
		/* r5 round2 A2: no card = no call: an unmute is stored (kernel default). A mute is still refused: stored,
		 * it would block every later call (fail closed) on a kernel without the TX Mute control. */
		if (r == -ENODEV && m == 0)
			r = 0;
		if (r < 0) {
			snprintf(reply, n, "ERR %d %s", -r, strerror(-r));
		} else {
			mute_want = m;
			snprintf(reply, n, "OK mute=%d", m);
			/* btcall: the injected headset mic is muted by disconnecting the uplink route */
			if (bt.enabled && (r = bt_sync_routes(v->card, mono_ms(), 1)) < 0 && m && bt.active)
				snprintf(reply, n, "ERR %d btcall uplink route not cleared: %s", -r, strerror(-r));
		}
		q6_info("A6L_Q6VOICED mute %d -> %s\n", m, reply);
	} else if (!strncmp(line, "volume ", 7)) {
		/* r5 round4 F37: Android voice volume 0..1 -> step 0..vol_max_step (NaN / out of range rejected) */
		char *end = NULL;
		double f = strtod(line + 7, &end);

		if (end == line + 7 || *end != '\0' || !(f >= 0.0 && f <= 1.0)) {
			snprintf(reply, n, "ERR %d volume must be 0..1", EINVAL);
		} else {
			int step = (int)(f * vol_max_step + 0.5);

			r = ctl_set(v->card, CTL_RX_VOL, step);
			if (r == -ENODEV)	/* r5 round2 A2: no card: stored, re-asserted before the next PCM open */
				r = 0;
			if (r < 0) {
				snprintf(reply, n, "ERR %d %s", -r, strerror(-r));
			} else {
				vol_want = step;
				snprintf(reply, n, "OK volume=%d", step);
			}
		}
		q6_info("A6L_Q6VOICED %s -> %s\n", line, reply);
	} else if (!strcmp(line, "getvolume")) {
		snprintf(reply, n, "OK volume=%d", vol_want);
	} else if (!strcmp(line, "getmute")) {
		snprintf(reply, n, "OK mute=%d", mute_want == 1);
	} else if (!strcmp(line, "status")) {
		snprintf(reply, n, "OK open=%d rx=%s mute=%d", v->tx >= 0, rx_name(v->rx_route), mute_want == 1);
		if (bt.enabled) {	/* btcall */
			size_t l = strlen(reply);
			snprintf(reply + l, n - l, " bridge=%d", bt.active);
		}
	} else {
		snprintf(reply, n, "ERR %d unknown command", EINVAL);
	}
}

static int ctl_socket_open(const char *path)
{
	const char *env = getenv("ANDROID_SOCKET_a6l_q6voiced");
	struct sockaddr_un sa;
	int fd;

	if (env && *env) {
		fd = atoi(env);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	} else if (path) {
		fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
			return -1;
		memset(&sa, 0, sizeof(sa));
		sa.sun_family = AF_UNIX;
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
		unlink(path);
		if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(fd);
			return -1;
		}
	} else {
		return -1;
	}
	if (listen(fd, 4) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void serve_one(int lfd, struct voice *v)
{
	struct timeval tv = { 1, 0 };
	char line[64], reply[128];
	ssize_t n;
	int c = accept(lfd, NULL, NULL);

	if (c < 0)
		return;
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	n = read(c, line, sizeof(line) - 1);
	if (n > 0) {
		line[n] = '\0';
		line[strcspn(line, "\r\n")] = '\0';
		handle_cmd(v, line, reply, sizeof(reply) - 1);
		strcat(reply, "\n");
		if (write(c, reply, strlen(reply)) < 0)
			q6_error( "A6L_Q6VOICED reply: %s\n", strerror(errno));
	}
	close(c);
}

static void usage(void)
{
	q6_error( "usage: a6l-q6voiced [-c card] [-d dev] [-r] [-s session] [-o outprop] [-S socket] [-V max_vol_step] [-b btcall_prefix] hold <sec> | daemon [prop] | routes <0|1> [speaker]\n");
	exit(2);
}

#ifndef A6L_Q6VOICED_NO_MAIN

int main(int argc, char **argv)
{
	struct voice v = { .card = 0, .dev = -1, .tx = -1, .rx = -1, .set_routes = 0, .rx_route = RX_EAR };
	const char *sock_path = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "c:d:rs:o:S:V:b:")) != -1) {
		switch (opt) {
		case 'c': v.card = atoi(optarg); break;
		case 'd': v.dev = atoi(optarg); break;
		case 'r': v.set_routes = 1; break;
		case 's': session_opt = optarg; break;
		case 'o': OUT_PROP = optarg; break;
		case 'S': sock_path = optarg; break;
		case 'b': bt_prefix = optarg; break;
		case 'V': vol_max_step = atoi(optarg); if (vol_max_step < 1 || vol_max_step > 15) usage(); break;
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
		return routes(v.card, atoi(argv[optind + 1]),
			      optind + 2 < argc && !strcmp(argv[optind + 2], "speaker") ? RX_SPK : RX_EAR) ? 1 : 0;
	}
	v.dev_auto = v.dev < 0;
	if (v.dev < 0)
		v.dev = find_voice_dev(v.card);
	if (v.dev < 0 && strcmp(argv[optind], "daemon")) {
		q6_error( "A6L_Q6VOICED no VoiceMMode1 pcm on card %d (/proc/asound/pcm)\n", v.card);
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
		struct retry rt = { 0, 0 };
		int lfd = ctl_socket_open(sock_path);

		signal(SIGPIPE, SIG_IGN);
		q6_info("A6L_Q6VOICED daemon card %d dev %d prop %s outprop %s socket %s\n", v.card, v.dev, prop, OUT_PROP,
		       lfd >= 0 ? "yes" : "NO (mute requests unavailable)");
		while (!stop_req) {
			int want = prop_active(prop);
			char out[92];
			struct pollfd pfd = { .fd = lfd, .events = POLLIN };
			int rx;

			prop_str(OUT_PROP, out, sizeof(out));
			bt_load_cfg();
			bt_step(&v, want, mono_ms());	/* btcall (default off) */
			/* btcall: bridged -> earpiece port (DSP clock; the codec is silenced by a6l-audio-route) */
			rx = bt.active ? RX_EAR : rx_route_for(out);
			daemon_step(&v, &rt, want, rx, mono_ms());	/* r5 F7 reopen + pass2 F19 retry */
			if (lfd >= 0) {
				if (poll(&pfd, 1, 250) > 0 && (pfd.revents & POLLIN))
					serve_one(lfd, &v);
			} else {
				struct timespec ts = { 0, 250 * 1000 * 1000 };
				nanosleep(&ts, NULL);
			}
		}
		voice_close(&v);
		return 0;
	}
	usage();
	return 2;
}
#endif /* A6L_Q6VOICED_NO_MAIN */
