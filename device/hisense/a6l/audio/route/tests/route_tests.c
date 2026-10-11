// SPDX-License-Identifier: Apache-2.0
/*
 * r5 review fix F7 (28 Sep 2026) + round6 (29 Sep 2026): host tests of a6l-audio-route. The daemon source is included
 * with main() compiled out and linked with the REAL libaudioroute (system/media/audio_route/audio_route.c of the Lineage
 * tree, expat) against a fake tinyalsa card whose controls are generated from the REAL mixer_paths_a6l.xml (every
 * <ctl> name; enum when a value is not numeric). So the XML, libaudioroute's value cache and the daemon's verification
 * are exercised together. Injected failures: writes of one control rejected (-EIO, which libaudioroute ignores), card
 * loss, codec reset with the card kept (all controls back to power-on values), card without the TFA9894.
 * Run: bash tests/run-tests.sh [<lineage tree>]   -> A6L_AUDIO_ROUTE_TESTS PASS
 */
#define A6L_AUDIO_ROUTE_NO_MAIN
#include "../a6l_audio_route.c"

static int g_pass, g_fail;
#define EXPECT(c) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static const char *XML = "../mixer_paths_a6l.xml";

/* ---------------------------------------------------------------- fake card (from the XML) */
struct mixer { int dummy; };
struct mixer_ctl { char name[64]; int type; char enums[16][48]; unsigned int nenum; int value; };
static struct mixer the_mixer;
static struct mixer_ctl CTLS[128];
static unsigned int NCTL;
static int card_dead, has_tfa = 1, writes;
static const char *lost_write;	/* control whose writes are rejected */
static char props[9][2][PROPERTY_VALUE_MAX];
static int has_att = 1;	/* audio-20261010: TFA module with "TFA Attenuation" (0004) */
static int att_writes;	/* audio-20261010: writes of "TFA Attenuation" (must stay 0 without a speakerphone call) */
static long long fake_now = 1000;
static long long fake_clock(void) { return fake_now; }

static int hidden(const struct mixer_ctl *c)
{
	if (!strcmp(c->name, "TFA Attenuation") && (!has_tfa || !has_att))
		return 1;
	return !has_tfa && (!strcmp(c->name, "TFA Profile") || !strncmp(c->name, "TERT_", 5));
}
static struct mixer_ctl *add_ctl(const char *name, int type)
{
	for (unsigned int i = 0; i < NCTL; i++)
		if (!strcmp(CTLS[i].name, name))
			return &CTLS[i];
	struct mixer_ctl *c = &CTLS[NCTL++];
	snprintf(c->name, sizeof(c->name), "%s", name);
	c->type = type;
	return c;
}
static void build_card(void)
{
	static struct xroute x;
	memset(CTLS, 0, sizeof(CTLS));
	NCTL = 0;
	add_ctl("Headphone Jack", MIXER_CTL_TYPE_BOOL);
	add_ctl("Mic Jack", MIXER_CTL_TYPE_BOOL);
	if (xr_load(&x, XML)) { fprintf(stderr, "cannot read %s\n", XML); exit(2); }
	for (int i = 0; i < x.nctl; i++) {
		const char *v = x.ctl[i].value;
		int num = (*v == '-' || (*v >= '0' && *v <= '9'));
		struct mixer_ctl *c = add_ctl(x.ctl[i].name, num ? MIXER_CTL_TYPE_INT : MIXER_CTL_TYPE_ENUM);
		if (!num) {
			if (!c->nenum) snprintf(c->enums[c->nenum++], 48, "ZERO");	/* power-on value first */
			unsigned int k;
			for (k = 0; k < c->nenum && strcmp(c->enums[k], v); k++) {}
			if (k == c->nenum) snprintf(c->enums[c->nenum++], 48, "%s", v);
		}
	}
}
static void codec_reset(void) { for (unsigned int i = 2; i < NCTL; i++) CTLS[i].value = 0; }
static struct mixer_ctl *ctl(const char *n)
{
	for (unsigned int i = 0; i < NCTL; i++)
		if (!strcmp(CTLS[i].name, n))
			return hidden(&CTLS[i]) ? NULL : &CTLS[i];
	return NULL;
}
static const char *val(const char *n)
{
	struct mixer_ctl *c = ctl(n);
	static char b[16];
	if (!c) return "?";
	if (c->type == MIXER_CTL_TYPE_ENUM) return c->enums[c->value];
	snprintf(b, sizeof(b), "%d", c->value);
	return b;
}
struct mixer *mixer_open(unsigned int card) { return (card == 0 && !card_dead) ? &the_mixer : NULL; }
void mixer_close(struct mixer *m) { (void)m; }
const char *mixer_get_name(struct mixer *m) { (void)m; return "Hisense A6L"; }
unsigned int mixer_get_num_ctls(struct mixer *m) { (void)m; unsigned int n = 0; for (unsigned int i = 0; i < NCTL; i++) n += !hidden(&CTLS[i]); return n; }
struct mixer_ctl *mixer_get_ctl(struct mixer *m, unsigned int id)
{
	(void)m;
	for (unsigned int i = 0; i < NCTL; i++)
		if (!hidden(&CTLS[i]) && id-- == 0)
			return &CTLS[i];
	return NULL;
}
struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *m, const char *name) { (void)m; return ctl(name); }
const char *mixer_ctl_get_name(struct mixer_ctl *c) { return c->name; }
enum mixer_ctl_type mixer_ctl_get_type(struct mixer_ctl *c) { return (enum mixer_ctl_type)c->type; }
unsigned int mixer_ctl_get_num_values(struct mixer_ctl *c) { (void)c; return 1; }
unsigned int mixer_ctl_get_num_enums(struct mixer_ctl *c) { return c->nenum; }
const char *mixer_ctl_get_enum_string(struct mixer_ctl *c, unsigned int i) { return i < c->nenum ? c->enums[i] : NULL; }
int mixer_ctl_get_value(struct mixer_ctl *c, unsigned int id) { (void)id; return card_dead ? -EINVAL : c->value; }
int mixer_ctl_is_access_tlv_rw(struct mixer_ctl *c) { (void)c; return 0; }
int mixer_ctl_get_array(struct mixer_ctl *c, void *a, size_t n) { if (card_dead) return -EINVAL; if (n) ((int *)a)[0] = c->value; return 0; }
int mixer_ctl_set_value(struct mixer_ctl *c, unsigned int id, int v)
{
	(void)id;
	if (card_dead || (lost_write && !strcmp(lost_write, c->name))) return -EIO;
	/* q6routing has ONE RX backend per FE, not independent boolean switches.
	 * Match msm_routing_put_audio_mixer_dir(): enabling a backend replaces
	 * the previous one; clearing an inactive backend leaves the active one.
	 */
	const char *lpi = "LPI_MI2S_RX_0 Audio Mixer MultiMedia1";
	const char *tert = "TERT_MI2S_RX Audio Mixer MultiMedia1";
	if (!strcmp(c->name, lpi) || !strcmp(c->name, tert)) {
		struct mixer_ctl *other = ctl(!strcmp(c->name, lpi) ? tert : lpi);
		if (v && other)
			other->value = 0;
	}
	c->value = v; writes++;
	if (!strcmp(c->name, "TFA Attenuation")) att_writes++;
	return 0;
}
int mixer_ctl_set_array(struct mixer_ctl *c, const void *a, size_t n) { return n ? mixer_ctl_set_value(c, 0, ((const int *)a)[0]) : 0; }

/* btcall (29 Sep 2026): bridge properties + fake /proc pcm status */
static char bt_bridge[PROPERTY_VALUE_MAX], bt_dl[PROPERTY_VALUE_MAX];
static int busy_ret, busy_calls, busy_dev, busy_cap;
static int fake_busy(int card, int dev, int cap) { (void)card; busy_calls++; busy_dev = dev; busy_cap = cap; return busy_ret; }

int property_get(const char *key, char *value, const char *def)
{
	if ((!strcmp(key, "persist.vendor.a6l.btcall.bridge") && bt_bridge[0]) ||
	    (!strcmp(key, "persist.vendor.a6l.btcall.dl_pcm") && bt_dl[0])) {
		snprintf(value, PROPERTY_VALUE_MAX, "%s", key[26] == 'b' ? bt_bridge : bt_dl);
		return (int)strlen(value);
	}
	static const char *keys[9] = { "vendor.a6l.voice.active", "vendor.a6l.audio.call_out", "vendor.a6l.audio.call_in",
				       "vendor.a6l.audio.media_out", "vendor.a6l.audio.media_in",
				       "vendor.a6l.audio.call_vol", "vendor.a6l.audio.call_vol_range",
				       "vendor.a6l.audio.call_boost_db", "ro.config.vc_call_vol_steps" };
	for (int i = 0; i < 9; i++)
		if (!strcmp(key, keys[i]) && props[i][1][0]) {
			snprintf(value, PROPERTY_VALUE_MAX, "%s", props[i][0]);
			return (int)strlen(value);
		}
	snprintf(value, PROPERTY_VALUE_MAX, "%s", def);
	return (int)strlen(value);
}
static void setprop(int i, const char *v) { snprintf(props[i][0], PROPERTY_VALUE_MAX, "%s", v); props[i][1][0] = 1; }
static void jack(int hp, int mic) { ctl("Headphone Jack")->value = hp; ctl("Mic Jack")->value = mic; }
#define STEP() route_step(&r, "Hisense A6L")

/* ---------------------------------------------------------------- tests */
static void test_decide(void)
{
	struct route_choice c;
	struct route_state s = { .hp = 0, .mic = 0, .voice = 0, .has_spk = 1, .call_out = "", .call_in = "" };

	decide(&s, &c); EXPECT(!strcmp(c.out, "speaker") && !strcmp(c.in, "main-mic") && !c.unsupported);
	s.hp = 1; s.mic = 1;
	decide(&s, &c); EXPECT(!strcmp(c.out, "headphones") && !strcmp(c.in, "headset-mic"));
	s.voice = 1; s.hp = 0; s.mic = 0;
	decide(&s, &c); EXPECT(!strcmp(c.out, "voice-handset") && !strcmp(c.in, "main-mic"));	/* unset: jack default */
	s.call_out = "speaker";
	decide(&s, &c); EXPECT(!strcmp(c.out, "voice-speaker") && !strcmp(c.in, "main-mic") && !c.unsupported);
	s.hp = 1; s.mic = 1; s.call_in = "";	/* speakerphone with a headset plugged: speaker + main mic */
	decide(&s, &c); EXPECT(!strcmp(c.out, "voice-speaker") && !strcmp(c.in, "main-mic"));
	s.call_out = "headset"; s.call_in = "headset";
	decide(&s, &c); EXPECT(!strcmp(c.out, "voice-headphones") && !strcmp(c.in, "headset-mic"));
	s.hp = 0; s.mic = 0;	/* stale headset property after an unplug */
	decide(&s, &c); EXPECT(!strcmp(c.out, "voice-handset") && !strcmp(c.in, "main-mic"));
	s.call_out = "earpiece"; s.hp = 1; s.call_in = "main";
	decide(&s, &c); EXPECT(!strcmp(c.out, "voice-handset") && !strcmp(c.in, "main-mic"));
	s.call_out = "bt-sco"; s.call_in = "bt-sco";	/* placeholder: unsupported, jack fallback */
	decide(&s, &c); EXPECT(c.unsupported && !strcmp(c.out, "voice-headphones"));
	s.call_out = "speaker"; s.has_spk = 0; s.call_in = "";	/* DT without the TFA */
	decide(&s, &c); EXPECT(c.unsupported && !strcmp(c.out, "voice-headphones"));

	/* r5 review F62: Android's device selection outside modem calls (NORMAL / IN_COMMUNICATION) */
	struct route_state m = { .hp = 1, .mic = 1, .voice = 0, .has_spk = 1, .call_out = "", .call_in = "",
				 .media_out = "speaker", .media_in = "main" };
	decide(&m, &c); EXPECT(!strcmp(c.out, "speaker") && !strcmp(c.in, "main-mic") && !c.unsupported);	/* reviewer case */
	m.media_out = "headset"; m.media_in = "headset";
	decide(&m, &c); EXPECT(!strcmp(c.out, "headphones") && !strcmp(c.in, "headset-mic") && !c.unsupported);
	m.media_out = "earpiece"; m.media_in = "main";	/* VoIP on the earpiece with a headset plugged */
	decide(&m, &c); EXPECT(!strcmp(c.out, "handset") && !strcmp(c.in, "main-mic"));
	m.hp = 0; m.mic = 0; m.media_out = "earpiece";
	decide(&m, &c); EXPECT(!strcmp(c.out, "handset"));
	m.media_out = "headset"; m.media_in = "headset";	/* stale after an unplug: the jack wins */
	decide(&m, &c); EXPECT(!strcmp(c.out, "speaker") && !strcmp(c.in, "main-mic"));
	m.hp = 1; m.mic = 1; m.media_out = "speaker,headset"; m.media_in = "";	/* duplicated ringtone/alarm */
	decide(&m, &c); EXPECT(!strcmp(c.out, "speaker") && c.unsupported && !strcmp(c.in, "headset-mic"));
	m.media_out = ""; m.media_in = "";	/* unset: jack default (unchanged behaviour) */
	decide(&m, &c); EXPECT(!strcmp(c.out, "headphones") && !strcmp(c.in, "headset-mic") && !c.unsupported);
	m.media_out = "speaker"; m.has_spk = 0;	/* DT without the TFA */
	decide(&m, &c); EXPECT(!strcmp(c.out, "headphones") && c.unsupported);
	m.has_spk = 1; m.voice = 1; m.call_out = "earpiece";	/* a modem call keeps priority over media devices */
	decide(&m, &c); EXPECT(!strcmp(c.out, "voice-handset"));
}

static void test_loop(void)
{
	struct routed r = { .card = -1, .last_hp = -2 };

	build_card();
	EXPECT(route_open(&r, 0, XML) == 0 && r.has_spk);
	EXPECT(STEP() == 0);	/* idle: speaker + main mic */
	EXPECT(!strcmp(val("TERT_MI2S_RX Audio Mixer MultiMedia1"), "1") && !strcmp(val("Digital DEC1 MUX"), "ADC1"));
	int w = writes;
	EXPECT(STEP() == 0 && writes == w);	/* nothing changed: no re-apply */
	/* call on the earpiece, then speakerphone chosen in the dialer */
	setprop(0, "1"); setprop(1, "earpiece"); setprop(2, "main");
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch") && !strcmp(val("TERT_MI2S_RX Audio Mixer MultiMedia1"), "0"));
	setprop(1, "speaker");
	EXPECT(STEP() == 0 && !strcmp(val("TFA Profile"), "voice") && !strcmp(val("EAR_S"), "ZERO"));

	/* injected mixer failure with UNCHANGED jack state: the earpiece write is lost -> error, not cached, retried */
	lost_write = "EAR_S";
	setprop(1, "earpiece");
	EXPECT(STEP() < 0);
	EXPECT(r.fails == 1 && strcmp(r.last_out_dev, "earpiece"));	/* still the speaker state */
	EXPECT(STEP() < 0);	/* retried although nothing changed */
	lost_write = NULL;
	EXPECT(STEP() == 0 && r.fails == 0 && !strcmp(val("EAR_S"), "Switch"));	/* repaired through libaudioroute's cache */
	EXPECT(!strcmp(r.last_out_dev, "earpiece"));
	w = writes;
	EXPECT(STEP() == 0 && writes == w);

	/* persistent failure with the card present: reopen after 3 attempts */
	lost_write = "HPHL";
	jack(1, 0);
	setprop(1, "headset");
	EXPECT(STEP() < 0);
	EXPECT(STEP() < 0);
	EXPECT(STEP() == 1);
	lost_write = NULL;
	route_close(&r);
	EXPECT(r.m == NULL && r.card == -1);
	EXPECT(route_open(&r, 0, XML) == 0);
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "Switch"));

	/* card loss (ADSP restart): reads fail and the card is gone -> reopen at once */
	card_dead = 1;
	EXPECT(STEP() == 1);
	route_close(&r);
	EXPECT(find_card("Hisense A6L") < 0);
	card_dead = 0;
	EXPECT(find_card("Hisense A6L") == 0);
	EXPECT(route_open(&r, 0, XML) == 0 && STEP() == 0);
	route_close(&r);

	/* r5 review F62: VoIP speakerphone / built-in mic with a 4-pole headset plugged, no modem call */
	setprop(0, "0"); setprop(1, ""); setprop(2, ""); jack(1, 1);
	EXPECT(route_open(&r, 0, XML) == 0);
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "Switch") && !strcmp(val("Digital DEC1 MUX"), "ADC2"));	/* unset: jack */
	setprop(3, "speaker"); setprop(4, "main");
	EXPECT(STEP() == 0 && !strcmp(val("TERT_MI2S_RX Audio Mixer MultiMedia1"), "1") && !strcmp(val("HPHL"), "ZERO") &&
	       !strcmp(val("Digital DEC1 MUX"), "ADC1"));
	setprop(3, "earpiece");
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch") && !strcmp(val("TERT_MI2S_RX Audio Mixer MultiMedia1"), "0"));
	setprop(3, "headset"); setprop(4, "headset");
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "Switch") && !strcmp(val("Digital DEC1 MUX"), "ADC2"));
	setprop(3, ""); setprop(4, "");
	route_close(&r);

	/* card without the TFA: a speakerphone request falls back and still succeeds */
	setprop(0, "1"); setprop(2, "main");
	has_tfa = 0;
	jack(0, 0);
	setprop(1, "speaker");
	EXPECT(route_open(&r, 0, XML) == 0 && !r.has_spk);
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch"));
	route_close(&r);
	has_tfa = 1;
}

/* round6 F7: the reviewer's case and every required control of the selected paths, lost then restored */
static void test_full_route(void)
{
	struct routed r = { .card = -1, .last_hp = -2 };
	static struct xroute x;

	build_card();
	codec_reset();
	EXPECT(!xr_load(&x, XML));
	setprop(0, "0"); setprop(1, ""); setprop(2, "");
	jack(1, 1);	/* headset with mic: headphones + headset-mic */
	lost_write = "ADC2 MUX";	/* reviewer: Digital DEC1 MUX=ADC2 succeeds, ADC2 MUX=INP2 does not */
	EXPECT(route_open(&r, 0, XML) == 0);
	EXPECT(STEP() < 0 && !strcmp(val("Digital DEC1 MUX"), "ADC2") && !strcmp(val("ADC2 MUX"), "ZERO"));
	lost_write = NULL;
	EXPECT(STEP() == 0 && !strcmp(val("ADC2 MUX"), "INP2"));	/* automatic repair, no jack/call change */
	for (int i = 0; i < 100; i++) EXPECT(STEP() == 0);
	EXPECT(!strcmp(val("ADC2 MUX"), "INP2"));
	route_close(&r);

	/* each control of each selected path (and of the defaults they reset) lost independently, then restored */
	struct { int hp, mic, voice; const char *co, *out, *in; } cases[] = {
		{ 1, 1, 0, "", "headphones", "headset-mic" },
		{ 0, 0, 0, "", "speaker", "main-mic" },
		{ 1, 1, 1, "headset", "voice-headphones", "headset-mic" },
		{ 0, 0, 1, "speaker", "voice-speaker", "main-mic" },
		{ 0, 0, 1, "earpiece", "voice-handset", "main-mic" },
		{ 1, 1, 0, "M:speaker", "speaker", "main-mic" },	/* F62: media devices ("M:" = media_out) */
		{ 1, 1, 0, "M:earpiece", "handset", "main-mic" },
	};
	int tested = 0;
	for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		static struct xr_want wl[XR_MAX_CTL];
		int n = 0;
		xr_overlay(&x, cases[k].out, wl, &n, 0);
		xr_overlay(&x, cases[k].in, wl, &n, 0);
		for (int i = 0; i < n; i++) {
			/* start from a DIFFERENT committed route so the path's controls must change */
			build_card(); codec_reset();
			setprop(0, "0"); setprop(1, ""); setprop(2, ""); setprop(3, ""); setprop(4, ""); jack(0, 0);
			EXPECT(route_open(&r, 0, XML) == 0 && STEP() == 0);
			if (!strcmp(wl[i].c->value, val(wl[i].c->name))) { route_close(&r); continue; }	/* no change needed */
			lost_write = wl[i].c->name;
			int media = !strncmp(cases[k].co, "M:", 2);
			jack(cases[k].hp, cases[k].mic); setprop(0, cases[k].voice ? "1" : "0");
			setprop(1, media ? "" : cases[k].co); setprop(3, media ? cases[k].co + 2 : ""); setprop(4, media ? "main" : "");
			int r1 = STEP();
			lost_write = NULL;
			int r2 = STEP();
			int ok = r1 < 0 && r2 == 0 && verify_route(&x, &the_mixer, cases[k].out, cases[k].in) == 0;
			if (!ok) fprintf(stderr, "  case %s/%s control '%s': r1=%d r2=%d\n", cases[k].out, cases[k].in, wl[i].c->name, r1, r2);
			EXPECT(ok);
			tested++;
			route_close(&r);
		}
	}
	EXPECT(tested >= 15);
	fprintf(stderr, "I %d lost-control cases\n", tested);

	/* a SHARED control reset by the new route (headphones -> speaker must switch HPHL back off) */
	build_card(); codec_reset();
	setprop(0, "0"); setprop(1, ""); jack(1, 0);
	EXPECT(route_open(&r, 0, XML) == 0 && STEP() == 0 && !strcmp(val("HPHL"), "Switch"));
	lost_write = "HPHL";
	jack(0, 0);
	EXPECT(STEP() < 0 && !strcmp(val("HPHL"), "Switch"));	/* speaker set, headphone amp still on: not accepted */
	lost_write = NULL;
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "ZERO"));
	route_close(&r);

	/* codec/DSP reset that keeps the card enumerated: detected by the periodic re-verification and repaired */
	build_card(); codec_reset();
	jack(1, 1);
	EXPECT(route_open(&r, 0, XML) == 0 && STEP() == 0 && !strcmp(val("ADC2 MUX"), "INP2"));
	codec_reset();
	jack(1, 1);
	int repaired = -1;
	for (int i = 0; i < VERIFY_EVERY + 2 && repaired < 0; i++)
		if (STEP() == 0 && !strcmp(val("ADC2 MUX"), "INP2") && !strcmp(val("HPHL"), "Switch")) repaired = i;
	EXPECT(repaired >= 0 && repaired <= VERIFY_EVERY);
	EXPECT(verify_route(&x, &the_mixer, "headphones", "headset-mic") == 0);
	route_close(&r);
}

/* btcall (29 Sep 2026): call carried to a BT headset by the AudioFlinger bridge -> phone codec silent */
static void test_btcall(void)
{
	struct routed r = { .card = -1, .last_hp = -2 };
	struct route_state s = { .hp = 0, .mic = 0, .voice = 1, .has_spk = 1, .call_out = "speaker", .call_in = "main",
				 .bridge = 1 };
	struct route_choice c;

	decide(&s, &c);
	EXPECT(!strcmp(c.out, "voice-bridge") && !strcmp(c.in, "bridge-mic") && !c.unsupported);
	s.voice = 0;	/* outside a modem call the bridge flag is meaningless */
	s.media_out = "speaker";
	s.media_in = "";
	decide(&s, &c);
	EXPECT(!strcmp(c.out, "speaker"));

	build_card();
	jack(0, 0);
	lost_write = NULL;
	card_dead = 0;
	memset(props, 0, sizeof(props));
	pcm_busy = fake_busy;
	EXPECT(route_open(&r, 0, XML) == 0);
	setprop(0, "1"); setprop(1, "earpiece"); setprop(2, "main");
	busy_ret = 1;
	busy_calls = 0;
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch"));
	EXPECT(busy_calls == 0);	/* bridge property unset: /proc never read */
	snprintf(bt_bridge, sizeof(bt_bridge), "1");
	snprintf(bt_dl, sizeof(bt_dl), "3");
	busy_ret = 0;			/* enabled, but the HAL does not read the downlink (call on the phone) */
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch"));
	EXPECT(busy_calls >= 1 && busy_dev == 3 && busy_cap == 1);
	busy_ret = 1;			/* Android moved the call to the headset: the bridge streams run */
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "ZERO") && !strcmp(val("HPHL"), "ZERO") &&
	       !strcmp(val("Digital DEC1 MUX"), "ZERO") && !strcmp(val("TERT_MI2S_RX Audio Mixer MultiMedia1"), "0"));
	EXPECT(r.cur_out && !strcmp(r.cur_out, "voice-bridge") && !strcmp(r.cur_in, "bridge-mic"));
	int w = writes;
	EXPECT(STEP() == 0 && writes == w);	/* stable */
	jack(1, 1);			/* a wired headset plugged during the bridged call: still silent */
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "ZERO") && !strcmp(val("Digital DEC1 MUX"), "ZERO"));
	jack(0, 0);
	busy_ret = 0;			/* headset disconnected: back to the earpiece */
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch") && !strcmp(val("Digital DEC1 MUX"), "ADC1"));
	busy_ret = 1;
	snprintf(bt_dl, sizeof(bt_dl), "0");	/* invalid pcm number (MultiMedia1): never a bridge */
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch"));
	snprintf(bt_dl, sizeof(bt_dl), "3");
	snprintf(bt_bridge, sizeof(bt_bridge), "0");
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch"));
	route_close(&r);
	bt_bridge[0] = bt_dl[0] = '\0';
	pcm_busy = pcm_busy_real;
	memset(props, 0, sizeof(props));
}

/* ---- audio-20261010: in-call volume as an output gain, unplug grace */
static int codec_db(const char *v, const char *rg, const char *b, const char *n)
{
	return call_gain_db(v, rg, b, n).codec_db;
}
static int tfa_att(const char *v, const char *rg, const char *b, const char *n)
{
	return call_gain_db(v, rg, b, n).tfa_att_db;
}

static void test_call_gain_curve(void)
{
	/* unset / invalid volume: no call gain at all (the level before audio-20261010) */
	EXPECT(codec_db("", "", "", "") == 0 && tfa_att("", "", "", "") == 0);
	EXPECT(codec_db("1.5", "", "", "") == 0 && codec_db("abc", "", "", "") == 0 && codec_db("nan", "", "", "") == 0);
	EXPECT(tfa_att("1.5", "", "", "") == 0 && tfa_att("nan", "", "", "") == 0);
	/* 5 steps (AudioService default), defaults range 20 / boost 8: codec +8/0/-4/-8/-12, TFA 0/4/8/12/16 */
	static const char *v5[] = { "1.0000", "0.8000", "0.6000", "0.4000", "0.2000" };
	static const int c5[] = { 8, 0, -4, -8, -12 }, t5[] = { 0, 4, 8, 12, 16 };
	for (int k = 0; k < 5; k++) {
		EXPECT(codec_db(v5[k], "", "", "") == c5[k]);
		EXPECT(tfa_att(v5[k], "", "", "") == t5[k]);
		EXPECT(codec_db(v5[k], "", "", "5") == c5[k]);	/* explicit ro.config.vc_call_vol_steps=5 */
	}
	EXPECT(codec_db("0", "", "", "") == -16 && tfa_att("0", "", "", "") == 20);	/* index 0 (not reachable in calls) */
	/* the TFA (attenuation only) keeps the top step at full level whatever the boost */
	EXPECT(tfa_att("1.0000", "", "20", "") == 0 && tfa_att("1.0000", "30", "0", "") == 0);
	/* boost property: 0 = the old top level, 12, the hardware max 40; out of 0..40 or garbage: default 8 */
	EXPECT(codec_db("1.0000", "", "0", "") == 0 && codec_db("0.8000", "", "0", "") == 0);
	EXPECT(codec_db("1.0000", "", "12", "") == 12 && codec_db("0.8000", "", "12", "") == 0);
	EXPECT(codec_db("1.0000", "", "40", "") == 40);
	EXPECT(codec_db("1.0000", "", "41", "") == 8 && codec_db("1.0000", "", "-3", "") == 8 &&
	       codec_db("1.0000", "", "x", "") == 8);
	EXPECT(codec_db("1.0000", "", "6.4", "") == 6);
	/* the boost touches the top step only */
	EXPECT(codec_db("0.6000", "", "20", "") == -4 && codec_db("0.2000", "", "0", "") == -12);
	/* range property: per step range/N; out of 0..60: default 20 */
	EXPECT(codec_db("0.6000", "30", "", "") == -6 && tfa_att("0.6000", "30", "", "") == 12);
	EXPECT(codec_db("0.2000", "0", "", "") == 0 && tfa_att("0.2000", "0", "", "") == 0);
	EXPECT(codec_db("0.6000", "99", "", "") == -4 && tfa_att("0.5", "99", "", "") == 10);
	/* 7 steps, from the property or inferred from the value (k/7 is not on the 5-step grid) */
	char b[16];
	static const int c7[] = { 8, 0, -3, -6, -9, -11, -14 };
	for (int k = 7; k >= 1; k--) {
		snprintf(b, sizeof(b), "%.4f", k / 7.0);
		EXPECT(codec_db(b, "", "", "7") == c7[7 - k]);
		EXPECT(codec_db(b, "", "", "") == c7[7 - k]);
		EXPECT(tfa_att(b, "", "", "") == (int)lround(20.0 * (1.0 - k / 7.0)));
	}
	/* range 28 with 7 steps: 4 dB per step (+8/0/-4/-8/...) */
	snprintf(b, sizeof(b), "%.4f", 5 / 7.0);
	EXPECT(codec_db(b, "28", "", "7") == -4);
	/* 6 steps need the property (3/6 = 1/2 would infer 2 steps); with it: +8/0/-3/-7/-10/-13 */
	static const int c6[] = { 8, 0, -3, -7, -10, -13 };
	for (int k = 6; k >= 1; k--) {
		snprintf(b, sizeof(b), "%.4f", k / 6.0);
		EXPECT(codec_db(b, "", "", "6") == c6[6 - k]);
	}
	EXPECT(call_steps(0.5, "") == 2 && call_steps(0.5, "6") == 6 && call_steps(0.6, "7") == 5);
	EXPECT(call_steps(0.123456, "") == 5 && call_steps(1.0, "") == 5 && call_steps(0.6, "0") == 5);
	/* strictly monotonic below the top on every Android grid k/N (N = 2..12, as q6voiced prints it), top = boost */
	for (int n = 2; n <= 12; n++) {
		char ns[16];
		int lc = 1000, lt = -1, mono = 1;
		snprintf(ns, sizeof(ns), "%d", n);
		for (int k = n; k >= 1; k--) {
			snprintf(b, sizeof(b), "%.4f", (double)k / n);
			int c = codec_db(b, "", "", ns), t = tfa_att(b, "", "", ns);
			if (c > lc || (k < n - 1 && c >= lc) || t < lt || (k == n) != (c == 8) || (k == n - 1 && c != 0) ||
			    (k == n) != (t == 0))
				mono = 0;
			lc = c;
			lt = t;
		}
		if (!mono) fprintf(stderr, "  curve not monotonic for %d steps\n", n);
		EXPECT(mono);
	}
	/* off-grid values (never sent by the audio policy) stay bounded: codec -60..+8 dB, TFA 0..60 dB */
	int bounded = 1;
	for (int i = 0; i <= 1000; i++) {
		snprintf(b, sizeof(b), "%.4f", i / 1000.0);
		int c = codec_db(b, "", "", ""), t = tfa_att(b, "", "", "");
		bounded &= c >= -60 && c <= 8 && t >= 0 && t <= 60;
	}
	EXPECT(bounded);
	/* clipping headroom: the boosted top step stays inside the WCD control range (84 + 40 = 124) */
	EXPECT(84 + codec_db("1.0000", "", "40", "") <= 124 && 84 + codec_db("1.0000", "", "", "") == 92);
}

static void test_call_volume(void)
{
	struct routed r = { .card = -1, .last_hp = -2 };
	static struct xroute x;

	build_card();
	codec_reset();
	EXPECT(!xr_load(&x, XML));
	memset(props, 0, sizeof(props));
	jack(0, 0);
	EXPECT(route_open(&r, 0, XML) == 0 && STEP() == 0);
	/* speakerphone call at Android voice volume 3/5 -> -8 dB on the TFA */
	setprop(0, "1"); setprop(1, "speaker"); setprop(2, "main"); setprop(5, "0.6000");
	EXPECT(STEP() == 0 && !strcmp(val("TFA Profile"), "voice") && !strcmp(val("TFA Attenuation"), "16"));
	struct call_gain g8 = { -4, 8 };
	EXPECT(verify_route_att(&x, &the_mixer, "voice-speaker", "main-mic", &g8) == 0);
	int w = writes;
	EXPECT(STEP() == 0 && writes == w);			/* unchanged: no writes */
	setprop(5, "1.0000");					/* volume up to max: speaker at full level, no boost */
	EXPECT(STEP() == 0 && !strcmp(val("TFA Attenuation"), "0") && !strcmp(val("Digital RX1 Digital Volume"), "0"));
	setprop(5, "0.2000");
	EXPECT(STEP() == 0 && !strcmp(val("TFA Attenuation"), "32"));
	/* the periodic re-verification keeps the call gain (it used to undo plain mixer writes) */
	for (int i = 0; i < 3 * VERIFY_EVERY; i++) EXPECT(STEP() == 0);
	EXPECT(!strcmp(val("TFA Attenuation"), "32"));
	/* someone else rewrites the gain: repaired to the call gain, not to the XML value */
	ctl("TFA Attenuation")->value = 0;
	int ok = 0;
	for (int i = 0; i < VERIFY_EVERY + 3; i++) { STEP(); if (!strcmp(val("TFA Attenuation"), "32")) ok = 1; }
	EXPECT(ok);
	/* earpiece: Digital RX1 relative to the path value (84 = 0 dB): 3/5 = -4 dB, top step +8 dB */
	setprop(1, "earpiece"); setprop(5, "0.6000");
	EXPECT(STEP() == 0 && !strcmp(val("EAR_S"), "Switch") && !strcmp(val("Digital RX1 Digital Volume"), "80") &&
	       !strcmp(val("TFA Attenuation"), "0"));
	setprop(5, "1.0000");
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "92") && !strcmp(val("TFA Attenuation"), "0"));
	for (int i = 0; i < 2 * VERIFY_EVERY; i++) EXPECT(STEP() == 0);	/* the boost is the verified state */
	EXPECT(!strcmp(val("Digital RX1 Digital Volume"), "92"));
	setprop(5, "0.8000");					/* one step down: the old full level */
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "84"));
	setprop(7, "12"); setprop(5, "1.0000");			/* boost property */
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "96"));
	setprop(7, "0");					/* boost 0: the pre-boost top level */
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "84"));
	setprop(7, "40");					/* the control's max */
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "124"));
	setprop(7, ""); setprop(5, "0.6000");
	/* wired headset: RX1 + RX2 */
	jack(1, 1); setprop(1, "headset"); setprop(2, "headset");
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "Switch") && !strcmp(val("Digital RX1 Digital Volume"), "80") &&
	       !strcmp(val("Digital RX2 Digital Volume"), "80") && !strcmp(val("Digital DEC1 MUX"), "ADC2"));
	setprop(6, "30");					/* range property: 6 dB per step, -6 dB at 3/5 */
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "78"));
	setprop(6, "");
	setprop(5, "1.0000");					/* headset top step: +8 dB on both channels */
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "92") && !strcmp(val("Digital RX2 Digital Volume"), "92"));
	setprop(8, "7"); setprop(5, "0.7143");			/* 7 call steps: 5/7 = -3 dB */
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "81") && !strcmp(val("Digital RX2 Digital Volume"), "81"));
	setprop(8, ""); setprop(5, "0.6000");
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "80"));
	/* earpiece again: libaudioroute may believe RX1 is already at its path value, our write still lands */
	setprop(1, "earpiece"); setprop(2, "main"); jack(0, 0);
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "80") && !strcmp(val("Digital RX2 Digital Volume"), "0"));
	/* call ends at the boosted top step: media paths get their XML values back (no call gain outside calls) */
	setprop(5, "1.0000"); setprop(7, "12");
	EXPECT(STEP() == 0 && !strcmp(val("Digital RX1 Digital Volume"), "96"));
	setprop(0, "0"); setprop(1, ""); setprop(2, ""); setprop(3, "headset"); jack(1, 0);
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "Switch") && !strcmp(val("Digital RX1 Digital Volume"), "84") &&
	       !strcmp(val("Digital RX2 Digital Volume"), "84"));	/* the PATH value (stock 0 dB), not the top level */
	setprop(3, "speaker"); jack(0, 0);
	EXPECT(STEP() == 0 && !strcmp(val("TFA Attenuation"), "0") && !strcmp(val("Digital RX1 Digital Volume"), "0"));
	w = writes;
	setprop(5, "0.4000");					/* volume change outside a call: nothing to do */
	EXPECT(STEP() == 0 && writes == w);
	setprop(7, "20");					/* boost change outside a call: nothing to do */
	EXPECT(STEP() == 0 && writes == w);
	setprop(7, "");
	route_close(&r);

	/* TFA module without the attenuation control: the speakerphone call still routes (gain skipped, warned) */
	has_att = 0;
	build_card(); codec_reset();
	memset(props, 0, sizeof(props));
	EXPECT(route_open(&r, 0, XML) == 0 && STEP() == 0);
	setprop(0, "1"); setprop(1, "speaker"); setprop(2, "main"); setprop(5, "0.2000");
	EXPECT(STEP() == 0 && !strcmp(val("TFA Profile"), "voice"));
	for (int i = 0; i < 2 * VERIFY_EVERY; i++) EXPECT(STEP() == 0);
	route_close(&r);
	has_att = 1;
	memset(props, 0, sizeof(props));
}

static void test_unplug_grace(void)
{
	struct routed r = { .card = -1, .last_hp = -2 };
	struct route_choice c;
	struct route_state m = { .hp = 0, .mic = 0, .voice = 0, .has_spk = 1, .hp_grace = 1, .call_out = "", .call_in = "",
				 .media_out = "headset", .media_in = "headset" };

	decide(&m, &c); EXPECT(!strcmp(c.out, "headphones") && !strcmp(c.in, "main-mic"));	/* silent tail, not the speaker */
	m.hp_grace = 0;
	decide(&m, &c); EXPECT(!strcmp(c.out, "speaker"));				/* stale after the grace */
	m.voice = 1; m.call_out = "headset"; m.call_in = "headset"; m.hp_grace = 1;
	decide(&m, &c); EXPECT(!strcmp(c.out, "voice-headphones") && !strcmp(c.in, "main-mic"));
	m.hp_grace = 0;
	decide(&m, &c); EXPECT(!strcmp(c.out, "voice-handset"));

	now_ms = fake_clock;
	build_card(); codec_reset();
	memset(props, 0, sizeof(props));
	jack(1, 1);
	setprop(3, "headset"); setprop(4, "headset");
	EXPECT(route_open(&r, 0, XML) == 0 && STEP() == 0 && !strcmp(val("HPHL"), "Switch"));
	jack(0, 0);						/* unplug while Android still routes to the headset */
	fake_now += 300;
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "Switch") && !strcmp(val("TERT_MI2S_RX Audio Mixer MultiMedia1"), "0"));
	fake_now += 700;
	setprop(3, "speaker"); setprop(4, "main");		/* AudioService disconnected the device: speaker */
	EXPECT(STEP() == 0 && !strcmp(val("TERT_MI2S_RX Audio Mixer MultiMedia1"), "1") && !strcmp(val("HPHL"), "ZERO"));
	/* stale property: speaker after the grace */
	jack(1, 0); setprop(3, "headset");
	fake_now += 100;
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "Switch"));
	jack(0, 0);
	fake_now += 100;
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "Switch"));
	fake_now += UNPLUG_GRACE_MS;
	EXPECT(STEP() == 0 && !strcmp(val("HPHL"), "ZERO") && !strcmp(val("TERT_MI2S_RX Audio Mixer MultiMedia1"), "1"));
	route_close(&r);
	now_ms = mono_ms_real;
	memset(props, 0, sizeof(props));
}

/* ---- audio-20261010 r2 (live test 10 Oct): "the path value is the baseline", "speaker media untouched" */
static int xml_value(const struct xroute *x, const char *out, const char *in, const char *name)
{
	static struct xr_want w[XR_MAX_CTL];
	int n = 0, v = -1;
	if (expected_state(x, out, in, w, &n))
		return -1;
	for (int i = 0; i < n; i++)
		if (!strcmp(w[i].c->name, name))
			v = atoi(w[i].c->value);
	return v;
}

static void test_path_baseline(void)
{
	struct routed r = { .card = -1, .last_hp = -2 };
	static struct xroute x;
	char b[16];

	build_card(); codec_reset();
	EXPECT(!xr_load(&x, XML));
	/* Pierre's rule: full stock level = raw 84 (0 dB) on every codec path, top level 0 = codec silent; media paths keep
	 * exactly these values, the call boost (+8 dB top step) exists only on the voice paths during a call */
	EXPECT(xml_value(&x, "headphones", "main-mic", "Digital RX1 Digital Volume") == 84);
	EXPECT(xml_value(&x, "headphones", "main-mic", "Digital RX2 Digital Volume") == 84);
	EXPECT(xml_value(&x, "handset", "main-mic", "Digital RX1 Digital Volume") == 84);
	EXPECT(xml_value(&x, "voice-headphones", "main-mic", "Digital RX1 Digital Volume") == 84);
	EXPECT(xml_value(&x, "voice-handset", "main-mic", "Digital RX1 Digital Volume") == 84);
	EXPECT(xml_value(&x, "speaker", "main-mic", "Digital RX1 Digital Volume") == 0);
	EXPECT(xml_value(&x, "speaker", "main-mic", "TFA Attenuation") == 0);
	memset(props, 0, sizeof(props));
	att_writes = 0;
	/* live-test sequence: speaker media, plug (headphones), call on the headset with the volume down, hang up,
	 * unplug (speaker), speaker call at max volume, hang up: after EVERY step the gain controls hold exactly the
	 * active path's XML value (or path value - att during a call) */
	struct { int hp, voice; const char *mo, *co, *vol, *out; int rx, tfa; } seq[] = {
		{ 0, 0, "speaker", "", "", "speaker", 0, 0 },
		{ 1, 0, "headset", "", "", "headphones", 84, 0 },
		{ 1, 1, "", "headset", "0.6000", "voice-headphones", 80, 0 },
		{ 1, 1, "", "headset", "1.0000", "voice-headphones", 92, 0 },	/* top step = +8 dB boost */
		{ 1, 1, "", "headset", "0.8000", "voice-headphones", 84, 0 },	/* one below = the old full level */
		{ 1, 0, "headset", "", "1.0000", "headphones", 84, 0 },
		{ 1, 1, "", "headset", "0.2000", "voice-headphones", 72, 0 },
		{ 1, 0, "headset", "", "0.2000", "headphones", 84, 0 },
		{ 0, 1, "", "earpiece", "1.0000", "voice-handset", 92, 0 },	/* earpiece top step: +8 dB */
		{ 0, 0, "speaker", "", "1.0000", "speaker", 0, 0 },		/* media: XML values only */
		{ 1, 1, "", "headset", "0.2000", "voice-headphones", 72, 0 },
		{ 1, 0, "headset", "", "0.2000", "headphones", 84, 0 },		/* call gain gone after the call */
		{ 0, 0, "speaker", "", "0.2000", "speaker", 0, 0 },
		{ 0, 0, "earpiece", "", "0.2000", "handset", 84, 0 },
		{ 1, 0, "headset", "", "0.2000", "headphones", 84, 0 },
	};
	EXPECT(route_open(&r, 0, XML) == 0);
	for (size_t k = 0; k < sizeof(seq) / sizeof(seq[0]); k++) {
		jack(seq[k].hp, 0);
		setprop(0, seq[k].voice ? "1" : "0"); setprop(1, seq[k].co); setprop(3, seq[k].mo); setprop(5, seq[k].vol);
		int ok = STEP() == 0 && r.cur_out && !strcmp(r.cur_out, seq[k].out);
		for (int i = 0; i < 2 * VERIFY_EVERY; i++)	/* re-verification keeps the values */
			ok &= STEP() == 0;
		snprintf(b, sizeof(b), "%d", seq[k].rx);
		const char *rx2 = (!strcmp(seq[k].out, "headphones") || !strcmp(seq[k].out, "voice-headphones")) ? b : "0";
		ok &= !strcmp(val("Digital RX1 Digital Volume"), b) && !strcmp(val("Digital RX2 Digital Volume"), rx2);
		snprintf(b, sizeof(b), "%d", seq[k].tfa);
		ok &= !strcmp(val("TFA Attenuation"), b);
		if (!ok)
			fprintf(stderr, "  baseline step %zu out=%s: RX1=%s RX2=%s TFA=%s\n", k, r.cur_out ? r.cur_out : "-",
				val("Digital RX1 Digital Volume"), val("Digital RX2 Digital Volume"), val("TFA Attenuation"));
		EXPECT(ok);
	}
	/* no speakerphone call happened: the TFA attenuation control was never written (the module stays inert) */
	EXPECT(att_writes == 0);
	/* speakerphone call at the top step: still never written (0 = the path value) */
	setprop(0, "1"); setprop(1, "speaker"); setprop(3, ""); setprop(5, "1.0000"); jack(0, 0);
	EXPECT(STEP() == 0 && !strcmp(val("TFA Profile"), "voice") && att_writes == 0);
	setprop(5, "0.8000");	/* one step down: -4 dB = 8 half-dB units, written once */
	EXPECT(STEP() == 0 && !strcmp(val("TFA Attenuation"), "8") && att_writes == 1);
	setprop(0, "0"); setprop(1, ""); setprop(3, "speaker");	/* hang up: back to 0 for media */
	EXPECT(STEP() == 0 && !strcmp(val("TFA Attenuation"), "0") && !strcmp(val("TFA Profile"), "music") && att_writes == 2);
	route_close(&r);
	memset(props, 0, sizeof(props));
}

int main(void)
{
	test_path_baseline();
	test_call_gain_curve();
	test_call_volume();
	test_unplug_grace();
	test_decide();
	test_loop();
	test_full_route();
	test_btcall();
	printf("A6L_AUDIO_ROUTE_TESTS %s pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
