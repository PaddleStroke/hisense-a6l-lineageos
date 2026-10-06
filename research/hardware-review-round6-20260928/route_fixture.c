// SPDX-License-Identifier: Apache-2.0
/*
 * r5 review fix F7 (28 Sep 2026): host tests of a6l-audio-route. The daemon source is included with main() compiled out,
 * against stub tinyalsa / libaudioroute / cutils headers (tests/stub) and the fake card below, which mirrors the
 * controls of mixer_paths_a6l.xml that the daemon reads back. Injected failures: a control write silently lost
 * (libaudioroute ignores write errors), card loss (all reads fail, card no longer enumerated).
 * Build/run: gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g -fsanitize=address,undefined \
 *            -Itests/stub -o /tmp/a6l-route-tests tests/route_tests.c && /tmp/a6l-route-tests
 */
#define A6L_AUDIO_ROUTE_NO_MAIN
#include "../a6l_audio_route.c"

static int g_pass, g_fail;
#define EXPECT(c) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------- fake card */
struct mixer { int dummy; };
struct mixer_ctl { const char *name; int is_enum; const char *enums[6]; int value; };
static struct mixer the_mixer;
static struct mixer_ctl CTLS[] = {
	{ "ADC2 MUX", 1, { "ZERO", "INP2", "INP3" }, 0 },
	{ "Headphone Jack", 0, { 0 }, 0 },
	{ "Mic Jack", 0, { 0 }, 0 },
	{ "HPHL", 1, { "ZERO", "Switch" }, 0 },
	{ "EAR_S", 1, { "ZERO", "Switch" }, 0 },
	{ "TERT_MI2S_RX Audio Mixer MultiMedia1", 0, { 0 }, 0 },
	{ "TFA Profile", 1, { "music", "voice", "ringtone", "bypass" }, 0 },
	{ "Digital DEC1 MUX", 1, { "ZERO", "ADC1", "ADC2", "ADC3" }, 0 },
};
#define NCTL (sizeof(CTLS) / sizeof(CTLS[0]))
static int card_dead, has_tfa = 1, applies;
static const char *lost_write;	/* control whose write is silently lost */
static char props[3][2][PROPERTY_VALUE_MAX];

static struct mixer_ctl *ctl(const char *n)
{
	for (size_t i = 0; i < NCTL; i++)
		if (!strcmp(CTLS[i].name, n))
			return (!has_tfa && (!strcmp(n, "TFA Profile") || !strncmp(n, "TERT_", 5))) ? NULL : &CTLS[i];
	return NULL;
}
struct mixer *mixer_open(unsigned int card) { return (card == 0 && !card_dead) ? &the_mixer : NULL; }
void mixer_close(struct mixer *m) { (void)m; }
const char *mixer_get_name(struct mixer *m) { (void)m; return "Hisense A6L"; }
struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *m, const char *name) { (void)m; return ctl(name); }
int mixer_ctl_get_value(const struct mixer_ctl *c, unsigned int id) { (void)id; return card_dead ? -EINVAL : c->value; }
enum mixer_ctl_type mixer_ctl_get_type(const struct mixer_ctl *c) { return c->is_enum ? MIXER_CTL_TYPE_ENUM : MIXER_CTL_TYPE_BOOL; }
const char *mixer_ctl_get_enum_string(struct mixer_ctl *c, unsigned int i) { return i < 6 ? c->enums[i] : NULL; }

static void set_enum(const char *n, const char *v)
{
	struct mixer_ctl *c = ctl(n);
	if (!c || card_dead || (lost_write && !strcmp(lost_write, n)))
		return;
	for (int i = 0; i < 6 && c->enums[i]; i++)
		if (!strcmp(c->enums[i], v))
			c->value = i;
}
static void set_int(const char *n, int v)
{
	struct mixer_ctl *c = ctl(n);
	if (c && !card_dead && !(lost_write && !strcmp(lost_write, n)))
		c->value = v;
}

/* fake libaudioroute: paths = the controls of mixer_paths_a6l.xml that matter here */
struct audio_route { char pend[2][32]; int n; };
static struct audio_route the_route;
struct audio_route *audio_route_init(unsigned int card, const char *xml) { (void)xml; return card == 0 ? &the_route : NULL; }
void audio_route_free(struct audio_route *ar) { (void)ar; }
void audio_route_reset(struct audio_route *ar) { ar->n = 0; }
int audio_route_apply_path(struct audio_route *ar, const char *name)
{
	static const char *known[] = { "headphones", "speaker", "voice-headphones", "voice-handset", "voice-speaker",
				       "main-mic", "headset-mic", "handset", "secondary-mic" };
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (!strcmp(known[i], name) && ar->n < 2) {
			snprintf(ar->pend[ar->n++], 32, "%s", name);
			return 0;
		}
	return -1;
}
int audio_route_update_mixer(struct audio_route *ar)
{
	applies++;
	/* reset state (top level of the XML) */
	set_enum("ADC2 MUX", "ZERO"); set_enum("HPHL", "ZERO"); set_enum("EAR_S", "ZERO"); set_int("TERT_MI2S_RX Audio Mixer MultiMedia1", 0);
	set_enum("TFA Profile", "music"); set_enum("Digital DEC1 MUX", "ZERO");
	for (int i = 0; i < ar->n; i++) {
		const char *p = ar->pend[i];
		if (!strcmp(p, "headphones") || !strcmp(p, "voice-headphones")) set_enum("HPHL", "Switch");
		if (!strcmp(p, "voice-handset")) set_enum("EAR_S", "Switch");
		if (!strcmp(p, "speaker")) { set_int("TERT_MI2S_RX Audio Mixer MultiMedia1", 1); set_enum("TFA Profile", "music"); }
		if (!strcmp(p, "voice-speaker")) set_enum("TFA Profile", "voice");
		if (!strcmp(p, "main-mic")) set_enum("Digital DEC1 MUX", "ADC1");
		if (!strcmp(p, "headset-mic")) { set_enum("Digital DEC1 MUX", "ADC2"); set_enum("ADC2 MUX", "INP2"); }
	}
	return 0;	/* like libaudioroute: failed writes are not reported */
}

int property_get(const char *key, char *value, const char *def)
{
	static const char *keys[3] = { "vendor.a6l.voice.active", "vendor.a6l.audio.call_out", "vendor.a6l.audio.call_in" };
	for (int i = 0; i < 3; i++)
		if (!strcmp(key, keys[i]) && props[i][1][0]) {
			snprintf(value, PROPERTY_VALUE_MAX, "%s", props[i][0]);
			return (int)strlen(value);
		}
	snprintf(value, PROPERTY_VALUE_MAX, "%s", def);
	return (int)strlen(value);
}
static void setprop(int i, const char *v) { snprintf(props[i][0], PROPERTY_VALUE_MAX, "%s", v); props[i][1][0] = 1; }

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
}

static void test_loop(void)
{
	struct routed r = { .card = -1, .last_hp = -2 };

	EXPECT(route_open(&r, 0, "x.xml") == 0 && r.has_spk);
	applies = 0;
	EXPECT(route_step(&r, "Hisense A6L") == 0 && applies == 1);	/* idle: speaker + main mic */
	EXPECT(CTLS[4].value == 1);
	EXPECT(route_step(&r, "Hisense A6L") == 0 && applies == 1);	/* nothing changed: no re-apply */
	/* call on the earpiece, then speakerphone chosen in the dialer */
	setprop(0, "1"); setprop(1, "earpiece"); setprop(2, "main");
	EXPECT(route_step(&r, "Hisense A6L") == 0 && applies == 2 && !strcmp(CTLS[3].enums[CTLS[3].value], "Switch"));
	setprop(1, "speaker");
	EXPECT(route_step(&r, "Hisense A6L") == 0 && applies == 3 && !strcmp(CTLS[5].enums[CTLS[5].value], "voice"));

	/* injected mixer failure with UNCHANGED jack state: the earpiece write is lost -> error, not cached, retried */
	lost_write = "EAR_S";
	setprop(1, "earpiece");
	EXPECT(route_step(&r, "Hisense A6L") < 0 && applies == 4);
	EXPECT(r.fails == 1 && strcmp(r.last_out_dev, "earpiece"));	/* still the speaker state */
	EXPECT(route_step(&r, "Hisense A6L") < 0 && applies == 5);		/* retried although nothing changed */
	lost_write = NULL;
	EXPECT(route_step(&r, "Hisense A6L") == 0 && applies == 6 && r.fails == 0);
	EXPECT(!strcmp(r.last_out_dev, "earpiece"));
	EXPECT(route_step(&r, "Hisense A6L") == 0 && applies == 6);

	/* persistent failure with the card present: reopen after 3 attempts */
	lost_write = "HPHL";
	CTLS[0].value = 1;	/* headset plugged */
	setprop(1, "headset");
	EXPECT(route_step(&r, "Hisense A6L") < 0);
	EXPECT(route_step(&r, "Hisense A6L") < 0);
	EXPECT(route_step(&r, "Hisense A6L") == 1);
	lost_write = NULL;
	route_close(&r);
	EXPECT(r.m == NULL && r.card == -1);
	EXPECT(route_open(&r, 0, "x.xml") == 0);
	EXPECT(route_step(&r, "Hisense A6L") == 0 && !strcmp(CTLS[2].enums[CTLS[2].value], "Switch"));

	/* card loss (ADSP restart): reads fail and the card is gone -> reopen at once */
	card_dead = 1;
	EXPECT(route_step(&r, "Hisense A6L") == 1);
	route_close(&r);
	EXPECT(find_card("Hisense A6L") < 0);
	card_dead = 0;
	EXPECT(find_card("Hisense A6L") == 0);
	EXPECT(route_open(&r, 0, "x.xml") == 0 && route_step(&r, "Hisense A6L") == 0);
	route_close(&r);

	/* card without the TFA: a speakerphone request falls back and still succeeds */
	has_tfa = 0;
	CTLS[0].value = 0;
	setprop(1, "speaker");
	EXPECT(route_open(&r, 0, "x.xml") == 0 && !r.has_spk);
	EXPECT(route_step(&r, "Hisense A6L") == 0 && !strcmp(CTLS[3].enums[CTLS[3].value], "Switch"));
	route_close(&r);
	has_tfa = 1;
}


#include <assert.h>
int main(void) {
    struct routed r = { .card = -1, .last_hp = -2 };
    ctl("Headphone Jack")->value = 1; ctl("Mic Jack")->value = 1;
    setprop(0, "1"); setprop(1, "headset"); setprop(2, "headset");
    assert(route_open(&r, 0, "x.xml") == 0);
    lost_write = "ADC2 MUX";
    assert(route_step(&r, "Hisense A6L") == 0);
    assert(ctl("ADC2 MUX")->value == 0 && applies == 1);
    assert(!strcmp(r.last_in_dev, "headset"));
    lost_write = NULL;
    for (int i=0; i<100; ++i) assert(route_step(&r, "Hisense A6L") == 0);
    assert(applies == 1 && ctl("ADC2 MUX")->value == 0);
    puts("PARTIAL_ROUTE_WRITE route_success=1 ADC2_MUX=ZERO applies=1 after_100_healthy_steps");
    route_close(&r);
}
