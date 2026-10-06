// SPDX-License-Identifier: Apache-2.0
/*
 * a6l-audio-route (audio3, 24 Sep 2026): mixer routing for the Hisense A6L "Hisense A6L" ASoC card.
 *
 * The AOSP AIDL example audio HAL (com.android.hardware.audio, module "primary") only opens ALSA card 0 / device 0 and
 * never sets mixer controls. This daemon owns the mixer instead: it loads /vendor/etc/mixer_paths_a6l.xml with
 * libaudioroute (the same parser the CAF/legacy HALs use), then follows the ASoC jack kcontrols of the card:
 *   "Headphone Jack" on  -> output path "headphones", otherwise "speaker" (or "headphones" when the card has no
 *                           TERT_MI2S speaker route, i.e. a DT without the TFA9894, so the output is never left dangling)
 *   "Mic Jack" on        -> input path "headset-mic", otherwise "main-mic"
 * The XML keeps one playback backend on MultiMedia1 (LPI_MI2S_RX_0, or TERT_MI2S_RX for speaker) and connects capture
 * on MultiMedia2 to LPI_MI2S_TX_3, so the HAL's pcmC0D0p / pcmC0D1c have backend DAIs. Polls every 300 ms.
 * rom-v2 (agent merge, 25 Sep 2026): in-call routing while vendor.a6l.voice.active=1 (set by the radio HAL during a
 * call; a6l-q6voiced then holds the VoiceMMode1 PCMs and the DSP voice routes).
 *
 * r5 review fix F7 (28 Sep 2026):
 *  - In a call the route follows ANDROID's choice, published by the audio HAL (tree patch audio/patches/0002, from the
 *    Telephony Rx / Telephony Tx audio patches the audio policy creates) in vendor.a6l.audio.call_out
 *    (speaker|earpiece|headset|headphone|bt-sco) and vendor.a6l.audio.call_in (main|headset|bt-sco):
 *      speaker   -> "voice-speaker" (TFA9894 voice profile; a6l-q6voiced moves the DSP voice RX port to TERT_MI2S_RX)
 *      earpiece  -> "voice-handset";  headset/headphone -> "voice-headphones" (only while the jack reports a plug)
 *      bt-sco    -> not supported yet (no HFP/SCO audio path): logged as unsupported, jack-based fallback
 *      unset     -> jack-based default (headset if plugged, else earpiece), the rom-v2 behaviour
 *  - apply() returns success; the cached state is only committed after a successful apply, so a failed route is
 *    retried on the next poll. libaudioroute ignores failed control writes, so the controls that define each path are
 *    read back. After 3 consecutive failures, or when the card index changed/disappeared (ADSP/card loss), the mixer
 *    and the route are released and the card is reopened.
 * r5 review round6 (29 Sep 2026, F7 was incomplete: one control per path proved nothing about the rest of the route):
 *  - the daemon reads mixer_paths XML itself and verifies the COMPLETE expected mixer state after every apply: the
 *    top-level defaults overlaid by the output path and then the input path (nested <path> references included), i.e.
 *    every control of both selected paths plus the shared/reset controls of the unselected ones. A control of a
 *    selected path that is missing or unreadable fails the route; a missing top-level-only control is skipped.
 *  - on any mismatch the libaudioroute instance is re-created before the retry: libaudioroute caches the value it
 *    TRIED to write (old_value = new_value even when mixer_ctl_set_* failed), so an identical re-apply would write
 *    nothing and the repair would never happen. audio_route_init() re-reads the hardware values.
 *  - the committed route is re-verified every VERIFY_EVERY polls (~3 s) although nothing changed, so a DSP/codec
 *    reset that keeps the card enumerated (controls back to power-on values) is detected and repaired.
 * r5 review F62 (28 Sep 2026): outside a modem call the route follows Android's devices for ordinary playback/capture
 * (NORMAL, VoIP/IN_COMMUNICATION, ringtone), published by the audio HAL in every mode (tree patch 0002) in
 * vendor.a6l.audio.media_out (comma list of speaker|earpiece|headset|headphone) and vendor.a6l.audio.media_in
 * (main|headset). The jack state is availability + fallback, not an override: speaker -> "speaker" even with a headset
 * plugged, earpiece -> "handset", headset/headphone -> "headphones" only while the jack reports a plug (stale property
 * after an unplug), main -> "main-mic", headset mic -> "headset-mic" only while "Mic Jack" is on. Speaker + headset
 * together (duplicated ringtone/alarm) has no combined path: "speaker" is used (audible) and reported as unsupported,
 * like bt-sco. Unset properties (no active patch, HAL without the patch) keep the jack-based behaviour. A modem call
 * (vendor.a6l.voice.active=1) keeps priority with call_out/call_in.
 * btcall (29 Sep 2026, docs/android-bt-audio-20260929.md): when a modem call is carried to a Bluetooth headset by the
 * AudioFlinger software bridge (persist.vendor.a6l.btcall.bridge=1 and the in-call record front end
 * pcmC<card>D<persist.vendor.a6l.btcall.dl_pcm>c is open, i.e. the audio HAL reads the downlink) the codec is switched
 * to "voice-bridge" / "bridge-mic" (every analog output and the ADC path off: nothing audible from the phone, no phone
 * mic in the uplink). The DSP voice session itself is untouched (a6l-q6voiced keeps its RX on the earpiece port).
 * usage: a6l-audio-route [-x mixer_paths.xml] [-n card_name] [-1]   (-1: apply once for the current state and exit)
 */
#define LOG_TAG "a6l-audio-route"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <tinyalsa/asoundlib.h>
#include <audio_route/audio_route.h>

#define DEFAULT_XML "/vendor/etc/mixer_paths_a6l.xml"
#define DEFAULT_CARD_NAME "Hisense A6L"
#define MAX_FAILS_BEFORE_REOPEN 3
#define VERIFY_EVERY 10		/* polls (300 ms) between re-verifications of an unchanged route */

static int find_card(const char *want)
{
	for (unsigned int c = 0; c < 8; c++) {
		struct mixer *m = mixer_open(c);
		if (!m)
			continue;
		const char *n = mixer_get_name(m);
		int match = n && !strcmp(n, want);
		mixer_close(m);
		if (match)
			return (int)c;
	}
	return -1;
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

/* returns 1/0 for a BOOL control, -1 if missing or unreadable */
static int get_bool(struct mixer *m, const char *name)
{
	struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, name);
	if (!ctl)
		return -1;
	int v = mixer_ctl_get_value(ctl, 0);
	if (v < 0)
		return -1;
	return v > 0 ? 1 : 0;
}

/* ---------------------------------------------------------------- decision (pure, host-tested) */
struct route_state {
	int hp, mic, voice, has_spk;
	int bridge;			/* btcall: modem call carried to a BT headset by the AudioFlinger bridge */
	const char *call_out, *call_in;	/* Android's call devices (audio HAL properties), "" = unknown */
	const char *media_out, *media_in;	/* F62: Android's playback/capture devices outside modem calls, "" = unknown */
};

struct route_choice {
	const char *out, *in;
	int unsupported;	/* Android asked for a device without an audio path (BT SCO): fallback used */
};

/* 1 if the comma-separated device list contains `dev` */
static int has_dev(const char *list, const char *dev)
{
	size_t n = strlen(dev);
	for (const char *p = list; p && *p;) {
		const char *e = strchr(p, ',');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l == n && !strncmp(p, dev, n))
			return 1;
		p = e ? e + 1 : NULL;
	}
	return 0;
}

static void decide(const struct route_state *s, struct route_choice *c)
{
	const char *co = s->call_out ? s->call_out : "", *ci = s->call_in ? s->call_in : "";

	c->unsupported = 0;
	c->in = (s->mic == 1) ? "headset-mic" : "main-mic";
	if (!s->voice) {
		/* r5 review F62: Android's selection outside modem calls; the jack is availability + fallback */
		const char *mo = s->media_out ? s->media_out : "", *mi = s->media_in ? s->media_in : "";
		int wired = has_dev(mo, "headset") || has_dev(mo, "headphone");
		c->out = (s->hp == 1 || !s->has_spk) ? "headphones" : "speaker";	/* jack-based default */
		if (has_dev(mo, "speaker")) {
			if (s->has_spk)
				c->out = "speaker";
			else
				c->unsupported = 1;
			if (wired)
				c->unsupported = 1;	/* speaker + headset: no combined path, speaker keeps it audible */
		} else if (wired) {
			c->out = (s->hp == 1 || !s->has_spk) ? "headphones" : "speaker";	/* stale after an unplug */
		} else if (has_dev(mo, "earpiece")) {
			c->out = "handset";
		}
		if (has_dev(mo, "bt-sco"))
			c->unsupported = 1;
		if (!strcmp(mi, "main"))
			c->in = "main-mic";
		else if (!strcmp(mi, "headset"))
			c->in = (s->mic == 1) ? "headset-mic" : "main-mic";
		else if (!strcmp(mi, "bt-sco"))
			c->unsupported = 1;
		return;
	}
	if (s->bridge) {	/* btcall: the call audio goes through the CPU to the headset: phone codec silent */
		c->out = "voice-bridge";
		c->in = "bridge-mic";
		return;
	}
	c->out = (s->hp == 1) ? "voice-headphones" : "voice-handset";	/* jack-based default */
	if (!strcmp(co, "speaker")) {
		if (s->has_spk)
			c->out = "voice-speaker";
		else
			c->unsupported = 1;
	} else if (!strcmp(co, "earpiece")) {
		c->out = "voice-handset";
	} else if (!strcmp(co, "headset") || !strcmp(co, "headphone")) {
		/* stale property after an unplug: the jack wins */
		c->out = (s->hp == 1) ? "voice-headphones" : "voice-handset";
	} else if (!strcmp(co, "bt-sco")) {
		c->unsupported = 1;	/* placeholder: HFP/SCO audio is not integrated */
	}
	if (!strcmp(ci, "main")) {
		c->in = "main-mic";
	} else if (!strcmp(ci, "headset")) {
		c->in = (s->mic == 1) ? "headset-mic" : "main-mic";
	} else if (!strcmp(ci, "bt-sco")) {
		c->unsupported = 1;
	}
	/* speakerphone: always the main (bottom) microphone, whatever the jack says about a headset mic */
	if (!strcmp(c->out, "voice-speaker") && strcmp(ci, "headset"))
		c->in = "main-mic";
}

/* ---------------------------------------------------------------- apply + verify */
/* ---------------------------------------------------------------- expected mixer state (from the XML) */
/* A minimal reader for the mixer_paths format used here: top-level <ctl name= value= [id=]/> (defaults), <path name=>
 * ... </path> blocks with <ctl/> and <path name=/> references. Comments are skipped. */
#define XR_MAX_CTL 512
#define XR_MAX_PATH 64
#define XR_MAX_INC 8
struct xr_ctl { char name[64], value[48]; int id; };	/* id -1 = all values */
struct xr_path { char name[64]; int first, n; char inc[XR_MAX_INC][64]; int ninc; };
struct xroute { struct xr_ctl ctl[XR_MAX_CTL]; int nctl; struct xr_path path[XR_MAX_PATH]; int npath; };

static int xr_attr(const char *tag, size_t len, const char *attr, char *out, size_t n)
{
	char key[24];
	snprintf(key, sizeof(key), " %s=\"", attr);
	const char *e = tag + len, *p = strstr(tag, key);
	if (!p || p >= e)
		return -1;
	p += strlen(key);
	const char *q = memchr(p, '"', (size_t)(e - p));
	if (!q || (size_t)(q - p) >= n)
		return -1;
	memcpy(out, p, (size_t)(q - p));
	out[q - p] = 0;
	return 0;
}

/* 0 on success; the whole file is read (it is small) */
static int xr_load(struct xroute *x, const char *xml)
{
	FILE *f = fopen(xml, "r");
	if (!f)
		return -errno;
	static char buf[128 * 1024];
	size_t len = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[len] = 0;
	memset(x, 0, sizeof(*x));
	struct xr_path *cur = NULL;
	for (char *p = buf; (p = strchr(p, '<'));) {
		if (!strncmp(p, "<!--", 4)) {
			char *c = strstr(p, "-->");
			if (!c)
				return -EINVAL;
			p = c + 3;
			continue;
		}
		char *e = strchr(p, '>');
		if (!e)
			return -EINVAL;
		size_t tl = (size_t)(e - p);
		int selfclose = e > p && e[-1] == '/';
		char name[64], value[48], id[16];
		if (!strncmp(p, "<ctl ", 5)) {
			if (x->nctl >= XR_MAX_CTL || xr_attr(p, tl, "name", name, sizeof(name)) ||
			    xr_attr(p, tl, "value", value, sizeof(value)))
				return -EINVAL;
			struct xr_ctl *c = &x->ctl[x->nctl++];
			snprintf(c->name, sizeof(c->name), "%s", name);
			snprintf(c->value, sizeof(c->value), "%s", value);
			c->id = xr_attr(p, tl, "id", id, sizeof(id)) ? -1 : atoi(id);
			if (cur)
				cur->n++;
		} else if (!strncmp(p, "<path ", 6)) {
			if (xr_attr(p, tl, "name", name, sizeof(name)))
				return -EINVAL;
			if (selfclose) {	/* reference to another path inside a path */
				if (!cur || cur->ninc >= XR_MAX_INC)
					return -EINVAL;
				snprintf(cur->inc[cur->ninc++], sizeof(cur->inc[0]), "%s", name);
			} else {
				if (cur || x->npath >= XR_MAX_PATH)
					return -EINVAL;
				cur = &x->path[x->npath++];
				snprintf(cur->name, sizeof(cur->name), "%s", name);
				cur->first = x->nctl;
			}
		} else if (!strncmp(p, "</path", 6)) {
			if (!cur)
				return -EINVAL;
			cur = NULL;
		}
		p = e + 1;
	}
	return cur ? -EINVAL : 0;
}

static const struct xr_path *xr_find(const struct xroute *x, const char *name)
{
	for (int i = 0; i < x->npath; i++)
		if (!strcmp(x->path[i].name, name))
			return &x->path[i];
	return NULL;
}

/* expected state entry: which XML ctl wins, and whether a selected path requires it */
struct xr_want { const struct xr_ctl *c; int required; };

static void xr_put(struct xr_want *w, int *n, const struct xr_ctl *c, int required)
{
	for (int i = 0; i < *n; i++)
		if (!strcmp(w[i].c->name, c->name) && w[i].c->id == c->id) {
			w[i].c = c;
			w[i].required |= required;
			return;
		}
	if (*n < XR_MAX_CTL) {
		w[*n].c = c;
		w[*n].required = required;
		(*n)++;
	}
}

static int xr_overlay(const struct xroute *x, const char *path, struct xr_want *w, int *n, int depth)
{
	const struct xr_path *p = xr_find(x, path);
	if (!p || depth > 4)
		return -ENOENT;
	for (int i = 0; i < p->ninc; i++)
		if (xr_overlay(x, p->inc[i], w, n, depth + 1))
			return -ENOENT;
	for (int i = 0; i < p->n; i++)
		xr_put(w, n, &x->ctl[p->first + i], 1);
	return 0;
}

/* 1 = matches, 0 = differs, -1 = missing/unreadable */
static int ctl_matches(struct mixer *m, const struct xr_ctl *c)
{
	struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, c->name);
	if (!ctl)
		return -1;
	if (mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_ENUM) {
		int v = mixer_ctl_get_value(ctl, 0);
		if (v < 0)
			return -1;
		const char *s = mixer_ctl_get_enum_string(ctl, (unsigned int)v);
		return s && !strcmp(s, c->value) ? 1 : 0;
	}
	unsigned int nv = mixer_ctl_get_num_values(ctl);
	int want = atoi(c->value);
	for (unsigned int i = 0; i < nv; i++) {
		if (c->id >= 0 && (int)i != c->id)
			continue;
		int v = mixer_ctl_get_value(ctl, i);
		if (v < 0)
			return -1;
		if (v != want)
			return 0;
	}
	return 1;
}

/* the complete expected mixer state for out+in: top-level defaults, then out, then in (libaudioroute's order) */
static int verify_route(const struct xroute *x, struct mixer *m, const char *out, const char *in)
{
	static struct xr_want w[XR_MAX_CTL];
	int n = 0, bad = 0;
	for (int i = 0; i < x->nctl; i++) {
		int inpath = 0;
		for (int k = 0; k < x->npath; k++)
			if (i >= x->path[k].first && i < x->path[k].first + x->path[k].n)
				inpath = 1;
		if (!inpath)
			xr_put(w, &n, &x->ctl[i], 0);
	}
	if (xr_overlay(x, out, w, &n, 0) || xr_overlay(x, in, w, &n, 0)) {
		ALOGE("verify: path %s/%s not in the XML", out, in);
		return -EINVAL;
	}
	for (int i = 0; i < n; i++) {
		int r = ctl_matches(m, w[i].c);
		if (r == 1 || (r < 0 && !w[i].required))
			continue;
		ALOGE("verify out=%s in=%s: '%s' %s (want %s)", out, in, w[i].c->name, r < 0 ? "missing/unreadable" : "differs",
		      w[i].c->value);
		bad++;
	}
	return bad ? -EIO : 0;
}

/* 0 on success, negative errno otherwise (nothing is cached by the caller then) */
static int apply(const struct xroute *x, struct audio_route *ar, struct mixer *m, const char *out, const char *in)
{
	audio_route_reset(ar);
	if (audio_route_apply_path(ar, out) < 0) {
		ALOGE("apply_path(%s) failed", out);
		return -EINVAL;
	}
	if (audio_route_apply_path(ar, in) < 0) {
		ALOGE("apply_path(%s) failed", in);
		return -EINVAL;
	}
	if (audio_route_update_mixer(ar) < 0) {
		ALOGE("update_mixer failed");
		return -EIO;
	}
	int r = verify_route(x, m, out, in);
	if (r) {
		printf("A6L_AUDIO_ROUTE_FAIL out=%s in=%s\n", out, in);
		fflush(stdout);
		return r;
	}
	ALOGI("route: out=%s in=%s", out, in);
	printf("A6L_AUDIO_ROUTE out=%s in=%s\n", out, in);
	fflush(stdout);
	return 0;
}

/* ---------------------------------------------------------------- loop */
struct routed {
	int card, has_spk;
	struct audio_route *ar;
	struct mixer *m;
	const char *xml;
	struct xroute *x;		/* expected states, from the same XML as libaudioroute */
	const char *cur_out, *cur_in;	/* committed paths (re-verified every VERIFY_EVERY polls) */
	int polls;
	int ar_stale;			/* libaudioroute's cache may not match the hardware: re-create before applying */
	/* committed (successfully applied) state; last_hp = -2 forces an apply */
	int last_hp, last_mic, last_voice, last_bridge;
	char last_out_dev[PROPERTY_VALUE_MAX], last_in_dev[PROPERTY_VALUE_MAX];
	char last_mout[PROPERTY_VALUE_MAX], last_min[PROPERTY_VALUE_MAX];	/* F62 */
	int fails;
};

static void route_close(struct routed *r)
{
	if (r->m)
		mixer_close(r->m);
	if (r->ar)
		audio_route_free(r->ar);
	free(r->x);
	r->x = NULL;
	r->m = NULL;
	r->ar = NULL;
	r->cur_out = r->cur_in = NULL;
	r->ar_stale = 0;
	r->card = -1;
	r->last_hp = -2;
	r->fails = 0;
}

static int route_open(struct routed *r, int card, const char *xml)
{
	r->xml = xml;
	r->x = malloc(sizeof(*r->x));
	int xe = r->x ? xr_load(r->x, xml) : -ENOMEM;
	if (xe) {
		ALOGE("cannot read the expected route states from %s: %s", xml, strerror(-xe));
		route_close(r);
		return -1;
	}
	r->ar = audio_route_init((unsigned int)card, xml);
	r->m = mixer_open((unsigned int)card);
	if (!r->ar || !r->m) {
		ALOGE("init failed (card %d, xml %s)", card, xml);
		route_close(r);
		return -1;
	}
	r->card = card;
	r->has_spk = mixer_get_ctl_by_name(r->m, "TERT_MI2S_RX Audio Mixer MultiMedia1") != NULL;
	r->last_hp = -2;
	r->fails = 0;
	ALOGI("card %d, speaker route %s", card, r->has_spk ? "present" : "absent (headphones fallback)");
	return 0;
}

/* round6 F7: libaudioroute caches the values it TRIED to write; a new instance re-reads the hardware so the next apply
 * rewrites every control that does not hold its value */
static int rebuild_ar(struct routed *r)
{
	audio_route_free(r->ar);
	r->ar = audio_route_init((unsigned int)r->card, r->xml);
	if (!r->ar)
		ALOGE("audio_route_init(card %d) failed: reopening", r->card);
	return r->ar ? 0 : -1;
}

/* one poll: 0 nothing to do / applied, <0 apply failed (state not committed), 1 card lost -> reopen needed */
static int route_step(struct routed *r, const char *cname)
{
	char vp[PROPERTY_VALUE_MAX], co[PROPERTY_VALUE_MAX], ci[PROPERTY_VALUE_MAX];
	char mo[PROPERTY_VALUE_MAX], mi[PROPERTY_VALUE_MAX];
	struct route_state s;
	struct route_choice c;

	property_get("vendor.a6l.voice.active", vp, "0");
	property_get("vendor.a6l.audio.call_out", co, "");
	property_get("vendor.a6l.audio.call_in", ci, "");
	property_get("vendor.a6l.audio.media_out", mo, "");
	property_get("vendor.a6l.audio.media_in", mi, "");
	s.hp = get_bool(r->m, "Headphone Jack");
	s.mic = get_bool(r->m, "Mic Jack");
	s.voice = vp[0] == '1';
	s.has_spk = r->has_spk;
	s.call_out = s.voice ? co : "";
	s.call_in = s.voice ? ci : "";
	s.media_out = s.voice ? "" : mo;	/* F62: a modem call keeps priority */
	s.media_in = s.voice ? "" : mi;
	s.bridge = 0;
	if (s.voice) {	/* btcall (default off): bridge enabled and the HAL's downlink stream running */
		char be[PROPERTY_VALUE_MAX], dp[PROPERTY_VALUE_MAX];
		int dl;

		property_get("persist.vendor.a6l.btcall.bridge", be, "0");
		property_get("persist.vendor.a6l.btcall.dl_pcm", dp, "-1");
		dl = atoi(dp);
		s.bridge = be[0] == '1' && dl > 0 && dl < 32 && pcm_busy(r->card, dl, 1);
	}
	if (s.hp == r->last_hp && s.mic == r->last_mic && s.voice == r->last_voice && s.bridge == r->last_bridge &&
	    !strcmp(s.call_out, r->last_out_dev) && !strcmp(s.call_in, r->last_in_dev) &&
	    !strcmp(s.media_out, r->last_mout) && !strcmp(s.media_in, r->last_min)) {
		/* round6 F7: an unchanged route is re-verified periodically (codec/DSP reset with the card kept) */
		if (!r->cur_out || ++r->polls < VERIFY_EVERY)
			return 0;
		r->polls = 0;
		if (!verify_route(r->x, r->m, r->cur_out, r->cur_in))
			return 0;
		ALOGW("committed route out=%s in=%s no longer applied: repairing", r->cur_out, r->cur_in);
		r->last_hp = -2;	/* not committed any more: re-applied below and on the next polls until it holds */
		r->cur_out = r->cur_in = NULL;
		r->ar_stale = 1;
	}
	decide(&s, &c);
	if (c.unsupported)
		ALOGW("%s device out='%s' in='%s' has no audio path (unsupported): using out=%s in=%s",
		      s.voice ? "call" : "media", s.voice ? s.call_out : s.media_out, s.voice ? s.call_in : s.media_in,
		      c.out, c.in);
	/* re-created right before the retry (not at failure time: audio_route_init() writes the XML defaults itself and
	 * would cache a still-failing write again) */
	if (r->ar_stale) {
		if (rebuild_ar(r))
			return 1;
		r->ar_stale = 0;
	}
	int ret = apply(r->x, r->ar, r->m, c.out, c.in);
	if (ret == 0) {
		r->cur_out = c.out;
		r->cur_in = c.in;
		r->polls = 0;
		r->last_hp = s.hp;
		r->last_mic = s.mic;
		r->last_voice = s.voice;
		r->last_bridge = s.bridge;
		snprintf(r->last_out_dev, sizeof(r->last_out_dev), "%s", s.call_out);
		snprintf(r->last_in_dev, sizeof(r->last_in_dev), "%s", s.call_in);
		snprintf(r->last_mout, sizeof(r->last_mout), "%s", s.media_out);
		snprintf(r->last_min, sizeof(r->last_min), "%s", s.media_in);
		r->fails = 0;
		return 0;
	}
	r->fails++;
	r->cur_out = r->cur_in = NULL;
	r->ar_stale = 1;	/* round6 F7: the retry must rewrite what did not take */
	if (r->fails >= MAX_FAILS_BEFORE_REOPEN || find_card(cname) != r->card) {
		ALOGE("route failed %d time(s), card %d %s: reopening", r->fails, r->card,
		      find_card(cname) == r->card ? "still present" : "lost");
		return 1;
	}
	return ret;
}

#ifndef A6L_AUDIO_ROUTE_NO_MAIN
int main(int argc, char **argv)
{
	const char *xml = DEFAULT_XML, *cname = DEFAULT_CARD_NAME;
	int once = 0, opt;
	struct routed r = { .card = -1, .last_hp = -2 };

	while ((opt = getopt(argc, argv, "x:n:1")) != -1) {
		switch (opt) {
		case 'x': xml = optarg; break;
		case 'n': cname = optarg; break;
		case '1': once = 1; break;
		default:
			fprintf(stderr, "usage: %s [-x mixer_paths.xml] [-n card_name] [-1]\n", argv[0]);
			return 2;
		}
	}
	for (;;) {
		/* the card appears once the ADSP and all ASoC modules are up; wait for it (forever for the service) */
		int card, waited = 0;
		while ((card = find_card(cname)) < 0) {
			if (waited++ % 60 == 0)
				ALOGI("waiting for card '%s'", cname);
			if (once && waited > 30) {
				fprintf(stderr, "A6L_AUDIO_ROUTE_FAIL no card '%s'\n", cname);
				return 3;
			}
			sleep(1);
		}
		if (route_open(&r, card, xml)) {
			if (once) {
				fprintf(stderr, "A6L_AUDIO_ROUTE_FAIL init card=%d xml=%s\n", card, xml);
				return 4;
			}
			sleep(1);
			continue;
		}
		for (;;) {
			int ret = route_step(&r, cname);
			if (once)
				return ret == 0 ? 0 : 5;
			if (ret == 1)
				break;
			usleep(300 * 1000);
		}
		route_close(&r);
	}
}
#endif
