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
static char props[5][2][PROPERTY_VALUE_MAX];

static int hidden(const struct mixer_ctl *c) { return !has_tfa && (!strcmp(c->name, "TFA Profile") || !strncmp(c->name, "TERT_", 5)); }
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
	static const char *keys[5] = { "vendor.a6l.voice.active", "vendor.a6l.audio.call_out", "vendor.a6l.audio.call_in",
				       "vendor.a6l.audio.media_out", "vendor.a6l.audio.media_in" };
	for (int i = 0; i < 5; i++)
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

int main(void)
{
	test_decide();
	test_loop();
	test_full_route();
	test_btcall();
	printf("A6L_AUDIO_ROUTE_TESTS %s pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
