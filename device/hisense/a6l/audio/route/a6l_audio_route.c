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
 * The top level of the XML keeps MultiMedia1 connected to LPI_MI2S_RX_0 / LPI_MI2S_TX_3 at all times, so the HAL's
 * pcmC0D0p/c opens never fail with "no backend DAIs" (EINVAL). Polls every 300 ms (no dependency on mixer event APIs).
 * Limitations: no simultaneous speaker+headset (ringtone duplication), no earpiece, no voice-call path.
 * usage: a6l-audio-route [-x mixer_paths.xml] [-n card_name] [-1]   (-1: apply once for the current jack state and exit)
 */
#define LOG_TAG "a6l-audio-route"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <log/log.h>
#include <tinyalsa/asoundlib.h>
#include <audio_route/audio_route.h>

#define DEFAULT_XML "/vendor/etc/mixer_paths_a6l.xml"
#define DEFAULT_CARD_NAME "Hisense A6L"

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

/* returns 1/0 for a BOOL control, -1 if missing */
static int get_bool(struct mixer *m, const char *name)
{
	struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, name);
	if (!ctl)
		return -1;
	return mixer_ctl_get_value(ctl, 0) > 0 ? 1 : 0;
}

static void apply(struct audio_route *ar, const char *out, const char *in)
{
	audio_route_reset(ar);
	if (audio_route_apply_path(ar, out) < 0)
		ALOGE("apply_path(%s) failed", out);
	if (audio_route_apply_path(ar, in) < 0)
		ALOGE("apply_path(%s) failed", in);
	if (audio_route_update_mixer(ar) < 0)
		ALOGE("update_mixer failed");
	ALOGI("route: out=%s in=%s", out, in);
	printf("A6L_AUDIO_ROUTE out=%s in=%s\n", out, in);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *xml = DEFAULT_XML, *cname = DEFAULT_CARD_NAME;
	int once = 0, opt, card = -1, waited = 0;

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
	/* the card appears once the ADSP and all ASoC modules are up; wait for it (forever for the service) */
	while ((card = find_card(cname)) < 0) {
		if (waited++ % 60 == 0)
			ALOGI("waiting for card '%s'", cname);
		if (once && waited > 30) {
			fprintf(stderr, "A6L_AUDIO_ROUTE_FAIL no card '%s'\n", cname);
			return 3;
		}
		sleep(1);
	}
	struct audio_route *ar = audio_route_init((unsigned int)card, xml);
	struct mixer *m = mixer_open((unsigned int)card);
	if (!ar || !m) {
		ALOGE("init failed (card %d, xml %s)", card, xml);
		fprintf(stderr, "A6L_AUDIO_ROUTE_FAIL init card=%d xml=%s\n", card, xml);
		return 4;
	}
	int has_spk = mixer_get_ctl_by_name(m, "TERT_MI2S_RX Audio Mixer MultiMedia1") != NULL;
	ALOGI("card %d '%s', speaker route %s", card, cname, has_spk ? "present" : "absent (headphones fallback)");
	int last_hp = -2, last_mic = -2;
	for (;;) {
		int hp = get_bool(m, "Headphone Jack"), mic = get_bool(m, "Mic Jack");
		if (hp != last_hp || mic != last_mic) {
			const char *out = (hp == 1 || !has_spk) ? "headphones" : "speaker";
			const char *in = (mic == 1) ? "headset-mic" : "main-mic";
			apply(ar, out, in);
			last_hp = hp;
			last_mic = mic;
		}
		if (once)
			break;
		usleep(300 * 1000);
	}
	mixer_close(m);
	audio_route_free(ar);
	return 0;
}
