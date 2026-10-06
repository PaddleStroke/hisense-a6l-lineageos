/*
 * Hisense A6L wpa_supplicant private driver-command library (BOARD_WPA_SUPPLICANT_PRIVATE_LIB) for mainline ath10k
 * over plain nl80211. r5 review fix F5 (agent wifi-hal, 29 Sep 2026). docs/wifi-hal-20260929.md.
 *
 * Without a private lib (ANDROID_LIB_STUB) every Android "DRIVER" command fails, including the framework's
 * ISupplicantStaIface.setCountryCode ("COUNTRY xx"), so cfg80211 would stay in the world domain in client mode.
 * This library implements exactly:
 *   COUNTRY xx  -> NL80211_CMD_REQ_SET_REG (cfg80211 user hint; the supplicant then gets NL80211_CMD_REG_CHANGE and
 *                  refreshes its channel list itself)
 *   MACADDR     -> current netdev address
 * Any other command returns -1, i.e. the same result as the stub build. The P2P hooks keep the stub build's return
 * values (driver_nl80211_android.c, ANDROID_LIB_STUB), so nothing else changes behaviour.
 *
 * This software may be distributed under the terms of the BSD license.
 */
#include "includes.h"

#include <linux/if.h>
#include <netlink/genl/genl.h>

#include "common.h"
#include "driver_nl80211.h"
#include "linux_ioctl.h"
#include "wpa_supplicant_i.h"

static int a6l_set_country(struct wpa_driver_nl80211_data *drv, const char *cc)
{
	char alpha2[3];
	struct nl_msg *msg;
	int i, ret;

	for (i = 0; i < 2; i++) {
		if (!isalnum((unsigned char) cc[i]))
			return -1;
		alpha2[i] = toupper((unsigned char) cc[i]);
	}
	if (cc[2] != '\0' && cc[2] != ' ')
		return -1;
	alpha2[2] = '\0';

	msg = nlmsg_alloc();
	if (!msg)
		return -1;
	if (!genlmsg_put(msg, 0, 0, drv->global->nl80211_id, 0, 0, NL80211_CMD_REQ_SET_REG, 0) ||
	    nla_put_string(msg, NL80211_ATTR_REG_ALPHA2, alpha2)) {
		nlmsg_free(msg);
		return -1;
	}
	ret = send_and_recv_msgs(drv, msg, NULL, NULL, NULL, NULL);
	wpa_printf(MSG_INFO, "a6l: COUNTRY %s -> REQ_SET_REG %s", alpha2, ret ? "failed" : "ok");
	return ret ? -1 : 0;
}

int wpa_driver_nl80211_driver_cmd(void *priv, char *cmd, char *buf, size_t buf_len)
{
	struct i802_bss *bss = priv;
	struct wpa_driver_nl80211_data *drv = bss->drv;

	if (os_strncasecmp(cmd, "COUNTRY ", 8) == 0)
		return a6l_set_country(drv, cmd + 8);

	if (os_strcasecmp(cmd, "MACADDR") == 0) {
		u8 macaddr[ETH_ALEN] = {};

		if (linux_get_ifhwaddr(drv->global->ioctl_sock, bss->ifname, macaddr) < 0)
			return -1;
		return os_snprintf(buf, buf_len, "Macaddr = " MACSTR "\n", MAC2STR(macaddr));
	}

	wpa_printf(MSG_DEBUG, "a6l: driver command '%s' not supported", cmd);
	return -1;
}

/* P2P hooks: identical to the ANDROID_LIB_STUB versions (P2P is not declared by this product). */
int wpa_driver_set_p2p_noa(void *priv, u8 count, int start, int duration)
{
	return 0;
}

int wpa_driver_get_p2p_noa(void *priv, u8 *buf, size_t len)
{
	return 0;
}

int wpa_driver_set_p2p_ps(void *priv, int legacy_ps, int opp_ps, int ctwindow)
{
	return -1;
}

int wpa_driver_set_ap_wps_p2p_ie(void *priv, const struct wpabuf *beacon,
				 const struct wpabuf *proberesp,
				 const struct wpabuf *assocresp)
{
	return 0;
}
