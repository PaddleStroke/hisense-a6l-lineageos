// SPDX-License-Identifier: MIT
/* r5 review round4 F37 host test: the real a6l-q6voiced request handling (socket + "volume" command) with the ALSA
 * control write replaced by a fake, served to the audio HAL helper under test (test-telephony-volume.sh). */
#define A6L_Q6VOICED_NO_MAIN
#include "../../kvoice/q6voiced/a6l_q6voiced.c"

long g_last_step = -1;
int g_fail_err;
static int fake_ctl(int card, const char *name, long v)
{
	(void)card;
	if (strcmp(name, "VoiceMMode1 RX Volume Step"))
		return 0;
	if (g_fail_err)
		return -g_fail_err;
	g_last_step = v;
	return 0;
}
int shim_listen(const char *path)
{
	ctl_write = fake_ctl;
	unlink(path);
	return ctl_socket_open(path);
}
void shim_serve_one(int lfd)
{
	struct voice v = { .card = 0, .dev = -1, .tx = -1, .rx = -1, .set_routes = 0, .rx_route = RX_EAR };

	serve_one(lfd, &v);
}
