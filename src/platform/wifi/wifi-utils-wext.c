/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2005 - 2011 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/ethernet.h>
#include <unistd.h>
#include <math.h>

#include <glib.h>

#include "wifi-utils-private.h"
#include "wifi-utils-wext.h"
#include "nm-logging.h"
#include "nm-utils.h"

/* Hacks necessary to #include wireless.h; yay for WEXT */
#ifndef __user
#define __user
#endif
#include <sys/types.h>
#include <linux/types.h>
#include <sys/socket.h>
#include <linux/wireless.h>


typedef struct {
	WifiData parent;
	int fd;
	struct iw_quality max_qual;
	gint8 num_freqs;
	guint32 freqs[IW_MAX_FREQUENCIES];
} WifiDataWext;

/* Until a new wireless-tools comes out that has the defs and the structure,
 * need to copy them here.
 */
/* Scan capability flags - in (struct iw_range *)->scan_capa */
#define NM_IW_SCAN_CAPA_NONE    0x00
#define NM_IW_SCAN_CAPA_ESSID   0x01

struct iw_range_with_scan_capa
{
	guint32 throughput;
	guint32 min_nwid;
	guint32 max_nwid;
	guint16 old_num_channels;
	guint8  old_num_frequency;

	guint8  scan_capa;
	/* don't need the rest... */
};

static guint32
iw_freq_to_uint32 (struct iw_freq *freq)
{
	if (freq->e == 0) {
		/* Some drivers report channel not frequency.  Convert to a
		 * frequency; but this assumes that the device is in b/g mode.
		 */
		if ((freq->m >= 1) && (freq->m <= 13))
			return 2407 + (5 * freq->m);
		else if (freq->m == 14)
			return 2484;
	}

	return (guint32) (((double) freq->m) * pow (10, freq->e) / 1000000);
}

static void
wifi_wext_deinit (WifiData *parent)
{
	WifiDataWext *wext = (WifiDataWext *) parent;

	if (wext->fd >= 0)
		close (wext->fd);
}

static NM80211Mode
wifi_wext_get_mode (WifiData *data)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;

	memset (&wrq, 0, sizeof (struct iwreq));
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);

	if (ioctl (wext->fd, SIOCGIWMODE, &wrq) < 0) {
		if (errno != ENODEV) {
			nm_log_warn (LOGD_HW | LOGD_WIFI,
			             "(%s): error %d getting card mode",
			             wext->parent.iface, errno);
		}
		return NM_802_11_MODE_UNKNOWN;
	}

	switch (wrq.u.mode) {
	case IW_MODE_ADHOC:
		return NM_802_11_MODE_ADHOC;
	case IW_MODE_MASTER:
		return NM_802_11_MODE_AP;
	case IW_MODE_INFRA:
		return NM_802_11_MODE_INFRA;
	default:
		break;
	}
	return NM_802_11_MODE_UNKNOWN;
}

static gboolean
wifi_wext_set_mode (WifiData *data, const NM80211Mode mode)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;

	if (wifi_wext_get_mode (data) == mode)
		return TRUE;

	memset (&wrq, 0, sizeof (struct iwreq));
	switch (mode) {
	case NM_802_11_MODE_ADHOC:
		wrq.u.mode = IW_MODE_ADHOC;
		break;
	case NM_802_11_MODE_AP:
		wrq.u.mode = IW_MODE_MASTER;
		break;
	case NM_802_11_MODE_INFRA:
		wrq.u.mode = IW_MODE_INFRA;
		break;
	default:
		g_warn_if_reached ();
		return FALSE;
	}

	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);
	if (ioctl (wext->fd, SIOCSIWMODE, &wrq) < 0) {
		if (errno != ENODEV) {
			nm_log_err (LOGD_HW | LOGD_WIFI, "(%s): error setting mode %d",
			            wext->parent.iface, mode);
		}
		return FALSE;
	}

	return TRUE;
}

static guint32
wifi_wext_get_freq (WifiData *data)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;

	memset (&wrq, 0, sizeof (struct iwreq));
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);
	if (ioctl (wext->fd, SIOCGIWFREQ, &wrq) < 0) {
		nm_log_warn (LOGD_HW | LOGD_WIFI,
		             "(%s): error getting frequency: %s",
		             wext->parent.iface, strerror (errno));
		return 0;
	}

	return iw_freq_to_uint32 (&wrq.u.freq);
}

static guint32
wifi_wext_find_freq (WifiData *data, const guint32 *freqs)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	int i;

	for (i = 0; i < wext->num_freqs; i++) {
		while (*freqs) {
			if (wext->freqs[i] == *freqs)
				return *freqs;
			freqs++;
		}
	}
	return 0;
}

static GByteArray *
wifi_wext_get_ssid (WifiData *data)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;
	char ssid[IW_ESSID_MAX_SIZE + 2];
	guint32 len;
	GByteArray *array = NULL;

	memset (ssid, 0, sizeof (ssid));
	wrq.u.essid.pointer = (caddr_t) &ssid;
	wrq.u.essid.length = sizeof (ssid);
	wrq.u.essid.flags = 0;
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);

	if (ioctl (wext->fd, SIOCGIWESSID, &wrq) < 0) {
		nm_log_err (LOGD_HW | LOGD_WIFI, "(%s): couldn't get SSID: %d",
		            wext->parent.iface, errno);
		return NULL;
	}

	len = wrq.u.essid.length;
	if (nm_utils_is_empty_ssid ((guint8 *) ssid, len) == FALSE) {
		array = g_byte_array_sized_new (len);
		g_byte_array_append (array, (const guint8 *) ssid, len);
	}

	return array;
}

static gboolean
wifi_wext_get_bssid (WifiData *data, guint8 *out_bssid)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;

	memset (&wrq, 0, sizeof (wrq));
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);
	if (ioctl (wext->fd, SIOCGIWAP, &wrq) < 0) {
		nm_log_warn (LOGD_HW | LOGD_WIFI,
		             "(%s): error getting associated BSSID: %s",
		             wext->parent.iface, strerror (errno));
		return FALSE;
	}
	memcpy (out_bssid, &(wrq.u.ap_addr.sa_data), ETH_ALEN);
	return TRUE;
}

static guint32
wifi_wext_get_rate (WifiData *data)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;
	int err;

	memset (&wrq, 0, sizeof (wrq));
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);
	err = ioctl (wext->fd, SIOCGIWRATE, &wrq);
	return ((err == 0) ? wrq.u.bitrate.value / 1000 : 0);
}

static int
wext_qual_to_percent (const struct iw_quality *qual,
                      const struct iw_quality *max_qual)
{
	int percent = -1;
	int level_percent = -1;

	g_return_val_if_fail (qual != NULL, -1);
	g_return_val_if_fail (max_qual != NULL, -1);

	/* Magically convert the many different WEXT quality representations to a percentage */

	nm_log_dbg (LOGD_WIFI,
	            "QL: qual %d/%u/0x%X, level %d/%u/0x%X, noise %d/%u/0x%X, updated: 0x%X  ** MAX: qual %d/%u/0x%X, level %d/%u/0x%X, noise %d/%u/0x%X, updated: 0x%X",
	            (__s8) qual->qual, qual->qual, qual->qual,
	            (__s8) qual->level, qual->level, qual->level,
	            (__s8) qual->noise, qual->noise, qual->noise,
	            qual->updated,
	            (__s8) max_qual->qual, max_qual->qual, max_qual->qual,
	            (__s8) max_qual->level, max_qual->level, max_qual->level,
	            (__s8) max_qual->noise, max_qual->noise, max_qual->noise,
	            max_qual->updated);

	/* Try using the card's idea of the signal quality first as long as it tells us what the max quality is.
	 * Drivers that fill in quality values MUST treat them as percentages, ie the "Link Quality" MUST be
	 * bounded by 0 and max_qual->qual, and MUST change in a linear fashion.  Within those bounds, drivers
	 * are free to use whatever they want to calculate "Link Quality".
	 */
	if ((max_qual->qual != 0) && !(max_qual->updated & IW_QUAL_QUAL_INVALID) && !(qual->updated & IW_QUAL_QUAL_INVALID))
		percent = (int)(100 * ((double)qual->qual / (double)max_qual->qual));

	/* If the driver doesn't specify a complete and valid quality, we have two options:
	 *
	 * 1) dBm: driver must specify max_qual->level = 0, and have valid values for
	 *        qual->level and (qual->noise OR max_qual->noise)
	 * 2) raw RSSI: driver must specify max_qual->level > 0, and have valid values for
	 *        qual->level and max_qual->level
	 *
	 * This is the WEXT spec.  If this interpretation is wrong, I'll fix it.  Otherwise,
	 * If drivers don't conform to it, they are wrong and need to be fixed.
	 */

	if (    (max_qual->level == 0) && !(max_qual->updated & IW_QUAL_LEVEL_INVALID)      /* Valid max_qual->level == 0 */
	    && !(qual->updated & IW_QUAL_LEVEL_INVALID)                                     /* Must have valid qual->level */
	    && (    ((max_qual->noise > 0) && !(max_qual->updated & IW_QUAL_NOISE_INVALID)) /* Must have valid max_qual->noise */
	        || ((qual->noise > 0) && !(qual->updated & IW_QUAL_NOISE_INVALID)))         /*    OR valid qual->noise */
	   ) {
		/* Absolute power values (dBm) */

		/* Reasonable fallbacks for dumb drivers that don't specify either level. */
		#define FALLBACK_NOISE_FLOOR_DBM  -90
		#define FALLBACK_SIGNAL_MAX_DBM   -20
		int max_level = FALLBACK_SIGNAL_MAX_DBM;
		int noise = FALLBACK_NOISE_FLOOR_DBM;
		int level = qual->level - 0x100;

		level = CLAMP (level, FALLBACK_NOISE_FLOOR_DBM, FALLBACK_SIGNAL_MAX_DBM);

		if ((qual->noise > 0) && !(qual->updated & IW_QUAL_NOISE_INVALID))
			noise = qual->noise - 0x100;
		else if ((max_qual->noise > 0) && !(max_qual->updated & IW_QUAL_NOISE_INVALID))
			noise = max_qual->noise - 0x100;
		noise = CLAMP (noise, FALLBACK_NOISE_FLOOR_DBM, FALLBACK_SIGNAL_MAX_DBM);

		/* A sort of signal-to-noise ratio calculation */
		level_percent = (int) (100 - 70 * (((double)max_level - (double)level) /
		                                   ((double)max_level - (double)noise)));
		nm_log_dbg (LOGD_WIFI, "QL1: level_percent is %d.  max_level %d, level %d, noise_floor %d.",
		            level_percent, max_level, level, noise);
	} else if (   (max_qual->level != 0)
	           && !(max_qual->updated & IW_QUAL_LEVEL_INVALID) /* Valid max_qual->level as upper bound */
	           && !(qual->updated & IW_QUAL_LEVEL_INVALID)) {
		/* Relative power values (RSSI) */

		int level = qual->level;

		/* Signal level is relavtive (0 -> max_qual->level) */
		level = CLAMP (level, 0, max_qual->level);
		level_percent = (int)(100 * ((double)level / (double)max_qual->level));
		nm_log_dbg (LOGD_WIFI, "QL2: level_percent is %d.  max_level %d, level %d.",
		            level_percent, max_qual->level, level);
	} else if (percent == -1) {
		nm_log_dbg (LOGD_WIFI, "QL: Could not get quality %% value from driver.  Driver is probably buggy.");
	}

	/* If the quality percent was 0 or doesn't exist, then try to use signal levels instead */
	if ((percent < 1) && (level_percent >= 0))
		percent = level_percent;

	nm_log_dbg (LOGD_WIFI, "QL: Final quality percent is %d (%d).",
	            percent, CLAMP (percent, 0, 100));
	return (CLAMP (percent, 0, 100));
}

static int
wifi_wext_get_qual (WifiData *data)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;
	struct iw_statistics stats;

	memset (&stats, 0, sizeof (stats));
	wrq.u.data.pointer = &stats;
	wrq.u.data.length = sizeof (stats);
	wrq.u.data.flags = 1;  /* Clear updated flag */
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);

	if (ioctl (wext->fd, SIOCGIWSTATS, &wrq) < 0) {
		nm_log_warn (LOGD_HW | LOGD_WIFI,
		             "(%s): error getting signal strength: %s",
		             wext->parent.iface, strerror (errno));
		return -1;
	}

	return wext_qual_to_percent (&stats.qual, &wext->max_qual);
}

/*********************/
/* OLPC Mesh-only functions */

static guint32
wifi_wext_get_mesh_channel (WifiData *data)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	guint32 freq;
	int i;

	freq = wifi_utils_get_freq (data);
	for (i = 0; i < wext->num_freqs; i++) {
		if (freq == wext->freqs[i])
			return i + 1;
	}
	return 0;
}

static gboolean
wifi_wext_set_mesh_channel (WifiData *data, guint32 channel)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;

	memset (&wrq, 0, sizeof (struct iwreq));
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);

	if (channel > 0) {
		wrq.u.freq.flags = IW_FREQ_FIXED;
		wrq.u.freq.e = 0;
		wrq.u.freq.m = channel;
	}

	if (ioctl (wext->fd, SIOCSIWFREQ, &wrq) < 0) {
		nm_log_err (LOGD_HW | LOGD_WIFI | LOGD_OLPC,
		            "(%s): error setting channel to %d: %s",
		            wext->parent.iface, channel, strerror (errno));
		return FALSE;
	}

	return TRUE;
}

static gboolean
wifi_wext_set_mesh_ssid (WifiData *data, const guint8 *ssid, gsize len)
{
	WifiDataWext *wext = (WifiDataWext *) data;
	struct iwreq wrq;
	char buf[IW_ESSID_MAX_SIZE + 1];

	memset (buf, 0, sizeof (buf));
	memcpy (buf, ssid, MIN (sizeof (buf) - 1, len));

	wrq.u.essid.pointer = (caddr_t) buf;
	wrq.u.essid.length = len;
	wrq.u.essid.flags = (len > 0) ? 1 : 0; /* 1=enable SSID, 0=disable/any */

	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);
	if (ioctl (wext->fd, SIOCSIWESSID, &wrq) == 0)
		return TRUE;

	if (errno != ENODEV) {
		nm_log_err (LOGD_HW | LOGD_WIFI | LOGD_OLPC,
		            "(%s): error setting SSID to '%s': %s",
		            wext->parent.iface,
		            ssid ? nm_utils_escape_ssid (ssid, len) : "(null)",
		            strerror (errno));
	}

	return FALSE;
}

/*********************/

static gboolean
wext_can_scan (WifiDataWext *wext)
{
	struct iwreq wrq;

	memset (&wrq, 0, sizeof (struct iwreq));
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);
	if (ioctl (wext->fd, SIOCSIWSCAN, &wrq) < 0) {
		if (errno == EOPNOTSUPP)
			return FALSE;
	}
	return TRUE;
}

static gboolean
wext_get_range (WifiDataWext *wext,
                struct iw_range *range,
                guint32 *response_len)
{
	int i = 26;
	gboolean success = FALSE;
	struct iwreq wrq;

	memset (&wrq, 0, sizeof (struct iwreq));
	strncpy (wrq.ifr_name, wext->parent.iface, IFNAMSIZ);
	wrq.u.data.pointer = (caddr_t) range;
	wrq.u.data.length = sizeof (struct iw_range);

	/* Need to give some drivers time to recover after suspend/resume
	 * (ex ipw3945 takes a few seconds to talk to its regulatory daemon;
	 * see rh bz#362421)
	 */
	while (i-- > 0) {
		if (ioctl (wext->fd, SIOCGIWRANGE, &wrq) == 0) {
			if (response_len)
				*response_len = wrq.u.data.length;
			success = TRUE;
			break;
		} else if (errno != EAGAIN) {
			nm_log_err (LOGD_HW | LOGD_WIFI,
			            "(%s): couldn't get driver range information (%d).",
			            wext->parent.iface, errno);
			break;
		}

		g_usleep (G_USEC_PER_SEC / 4);
	}

	if (i <= 0) {
		nm_log_warn (LOGD_HW | LOGD_WIFI,
		             "(%s): driver took too long to respond to IWRANGE query.",
		             wext->parent.iface);
	}

	return success;
}

#define WPA_CAPS (NM_WIFI_DEVICE_CAP_CIPHER_TKIP | \
                  NM_WIFI_DEVICE_CAP_CIPHER_CCMP | \
                  NM_WIFI_DEVICE_CAP_WPA | \
                  NM_WIFI_DEVICE_CAP_RSN)

static guint32
wext_get_caps (WifiDataWext *wext, struct iw_range *range)
{
	guint32 caps = NM_WIFI_DEVICE_CAP_NONE;

	g_return_val_if_fail (wext != NULL, NM_WIFI_DEVICE_CAP_NONE);
	g_return_val_if_fail (range != NULL, NM_WIFI_DEVICE_CAP_NONE);

	/* All drivers should support WEP by default */
	caps |= NM_WIFI_DEVICE_CAP_CIPHER_WEP40 | NM_WIFI_DEVICE_CAP_CIPHER_WEP104;

	if (range->enc_capa & IW_ENC_CAPA_CIPHER_TKIP)
		caps |= NM_WIFI_DEVICE_CAP_CIPHER_TKIP;

	if (range->enc_capa & IW_ENC_CAPA_CIPHER_CCMP)
		caps |= NM_WIFI_DEVICE_CAP_CIPHER_CCMP;

	if (range->enc_capa & IW_ENC_CAPA_WPA)
		caps |= NM_WIFI_DEVICE_CAP_WPA;

	if (range->enc_capa & IW_ENC_CAPA_WPA2)
		caps |= NM_WIFI_DEVICE_CAP_RSN;

	/* Check for cipher support but not WPA support */
	if (    (caps & (NM_WIFI_DEVICE_CAP_CIPHER_TKIP | NM_WIFI_DEVICE_CAP_CIPHER_CCMP))
	    && !(caps & (NM_WIFI_DEVICE_CAP_WPA | NM_WIFI_DEVICE_CAP_RSN))) {
		nm_log_warn (LOGD_WIFI, "%s: device supports WPA ciphers but not WPA protocol; "
		             "WPA unavailable.", wext->parent.iface);
		caps &= ~WPA_CAPS;
	}

	/* Check for WPA support but not cipher support */
	if (    (caps & (NM_WIFI_DEVICE_CAP_WPA | NM_WIFI_DEVICE_CAP_RSN))
	    && !(caps & (NM_WIFI_DEVICE_CAP_CIPHER_TKIP | NM_WIFI_DEVICE_CAP_CIPHER_CCMP))) {
		nm_log_warn (LOGD_WIFI, "%s: device supports WPA protocol but not WPA ciphers; "
		             "WPA unavailable.", wext->parent.iface);
		caps &= ~WPA_CAPS;
	}

	/* There's no way to detect Ad-Hoc/AP mode support with WEXT
	 * (other than actually trying to do it), so just assume that
	 * Ad-Hoc is supported and AP isn't.
	 */
	caps |= NM_WIFI_DEVICE_CAP_ADHOC;

	return caps;
}

WifiData *
wifi_wext_init (const char *iface, int ifindex, gboolean check_scan)
{
	WifiDataWext *wext;
	struct iw_range range;
	guint32 response_len = 0;
	struct iw_range_with_scan_capa *scan_capa_range;
	int i;

	wext = wifi_data_new (iface, ifindex, sizeof (*wext));
	wext->parent.get_mode = wifi_wext_get_mode;
	wext->parent.set_mode = wifi_wext_set_mode;
	wext->parent.get_freq = wifi_wext_get_freq;
	wext->parent.find_freq = wifi_wext_find_freq;
	wext->parent.get_ssid = wifi_wext_get_ssid;
	wext->parent.get_bssid = wifi_wext_get_bssid;
	wext->parent.get_rate = wifi_wext_get_rate;
	wext->parent.get_qual = wifi_wext_get_qual;
	wext->parent.deinit = wifi_wext_deinit;
	wext->parent.get_mesh_channel = wifi_wext_get_mesh_channel;
	wext->parent.set_mesh_channel = wifi_wext_set_mesh_channel;
	wext->parent.set_mesh_ssid = wifi_wext_set_mesh_ssid;

	wext->fd = socket (PF_INET, SOCK_DGRAM, 0);
	if (wext->fd < 0)
		goto error;

	memset (&range, 0, sizeof (struct iw_range));
	if (wext_get_range (wext, &range, &response_len) == FALSE) {
		nm_log_info (LOGD_HW | LOGD_WIFI, "(%s): driver WEXT range request failed",
		             wext->parent.iface);
		goto error;
	}

	if ((response_len < 300) || (range.we_version_compiled < 21)) {
		nm_log_info (LOGD_HW | LOGD_WIFI,
		             "(%s): driver WEXT version too old (got %d, expected >= 21)",
		             wext->parent.iface,
		             range.we_version_compiled);
		goto error;
	}

	wext->max_qual.qual = range.max_qual.qual;
	wext->max_qual.level = range.max_qual.level;
	wext->max_qual.noise = range.max_qual.noise;
	wext->max_qual.updated = range.max_qual.updated;

	wext->num_freqs = MIN (range.num_frequency, IW_MAX_FREQUENCIES);
	for (i = 0; i < wext->num_freqs; i++)
		wext->freqs[i] = iw_freq_to_uint32 (&range.freq[i]);

	/* Check for scanning capability; cards that can't scan are not supported */
	if (check_scan && (wext_can_scan (wext) == FALSE)) {
		nm_log_info (LOGD_HW | LOGD_WIFI,
		             "(%s): drivers that cannot scan are unsupported",
		             wext->parent.iface);
		goto error;
	}

	/* Check for the ability to scan specific SSIDs.  Until the scan_capa
	 * field gets added to wireless-tools, need to work around that by casting
	 * to the custom structure.
	 */
	scan_capa_range = (struct iw_range_with_scan_capa *) &range;
	if (scan_capa_range->scan_capa & NM_IW_SCAN_CAPA_ESSID) {
		nm_log_info (LOGD_HW | LOGD_WIFI,
		             "(%s): driver supports SSID scans (scan_capa 0x%02X).",
		             wext->parent.iface,
		             scan_capa_range->scan_capa);
	} else {
		nm_log_info (LOGD_HW | LOGD_WIFI,
		             "(%s): driver does not support SSID scans (scan_capa 0x%02X).",
		             wext->parent.iface,
		             scan_capa_range->scan_capa);
	}

	wext->parent.caps = wext_get_caps (wext, &range);

	nm_log_info (LOGD_HW | LOGD_WIFI,
	             "(%s): using WEXT for WiFi device control",
	             wext->parent.iface);

	return (WifiData *) wext;

error:
	wifi_utils_deinit ((WifiData *) wext);
	return NULL;
}

gboolean
wifi_wext_is_wifi (const char *iface)
{
	int fd;
	struct iwreq iwr;
	gboolean is_wifi = FALSE;

	fd = socket (PF_INET, SOCK_DGRAM, 0);
	if (fd >= 0) {
		strncpy (iwr.ifr_ifrn.ifrn_name, iface, IFNAMSIZ);
		if (ioctl (fd, SIOCGIWNAME, &iwr) == 0)
			is_wifi = TRUE;
		close (fd);
	}
	return is_wifi;
}
