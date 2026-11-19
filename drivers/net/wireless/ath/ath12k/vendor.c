// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* QCA nl80211 vendor commands of the QSDK cfg80211tool.
 *
 * Ported from the QSDK NHSS.QSDK.13.1.5-01089 ath12k driver
 * (oss/src/mac80211/wlan-open 3314b0461628, vendor.c): the generic Wi-Fi
 * (per interface) and wiphy (per radio) configuration commands and the
 * interface reload event they raise. Differences from the QSDK code:
 * - QCA_WLAN_VENDOR_ATTR_IF_OFFLOAD_TYPE returns -EOPNOTSUPP, as there is
 *   no PPE offload in this driver;
 * - the radio index and the parameter data length are checked;
 * - errors are logged through the wiphy, as ath12k_err() needs a device.
 *
 * The parameters marked QSDK 14.0 in vendor.h are ported from the
 * NHSS.QSDK.14.0-01668 driver (oss/src/qca-extns/wlan-open-extns
 * 42d2e21f19aa, ath/ath12k/src/vendor_extn.c).
 */

#include <net/netlink.h>
#include <net/mac80211.h>
#include "core.h"
#include "debug.h"
#include "mac.h"
#include "peer.h"
#include "vendor.h"

static const struct nla_policy
ath12k_wifi_config_policy[QCA_WLAN_VENDOR_ATTR_CONFIG_MAX + 1] = {
	[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND] = {.type = NLA_U32 },
	[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_VALUE] = {.type = NLA_U32 },
	[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_DATA] = {.type = NLA_BINARY },
	[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_LENGTH] = {.type = NLA_U32 },
	[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_FLAGS] = {.type = NLA_U32 },
	[QCA_WLAN_VENDOR_ATTR_CONFIG_IFINDEX] = {.type = NLA_U32 },
	[QCA_WLAN_VENDOR_ATTR_CONFIG_MLO_LINK_ID] = {.type = NLA_U8 },
	[QCA_WLAN_VENDOR_ATTR_IF_OFFLOAD_TYPE] = {.type = NLA_U8},
	[QCA_WLAN_VENDOR_ATTR_CONFIG_RADIO_INDEX] = {.type = NLA_U8 },
};

/* Send a 'reload' NL80211 event to userspace
 * link_id is 'invalid' for non-mlo wdev.
 * For mlo wdev, if 'invalid' link_id is passed, userspace will reload all
 * BSSes of the mlo
 * If valid link_id is passed for mlo wdev, only that BSS is reset
 * in userspace.
 */
static int ath12k_vendor_event_iface_reload_link(struct wiphy *wiphy,
						 struct wireless_dev *wdev,
						 u8 link_id)
{
	struct sk_buff *skb;

	skb = cfg80211_vendor_event_alloc(wiphy, wdev,
					  NLMSG_DEFAULT_SIZE,
					  QCA_NL80211_VENDOR_SUBCMD_IFACE_RELOAD_INDEX,
					  GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	if (nla_put_u8(skb, QCA_WLAN_VENDOR_ATTR_IFACE_RELOAD_LINKID, link_id)) {
		kfree_skb(skb);
		return -EINVAL;
	}

	ath12k_dbg(NULL, ATH12K_DBG_CFG,
		   "send event to userspace link_id %d\n", link_id);
	cfg80211_vendor_event(skb, GFP_KERNEL);

	return 0;
}

/* Find the wdevs corresponding to the radio of the wiphy
 * and send the reload event to userspace
 */
static int ath12k_vendor_event_iface_reload(struct wiphy *wiphy,
					    struct ath12k *ar)
{
	struct ath12k_link_vif *arvif;
	struct wireless_dev *wdev;

	list_for_each_entry(arvif, &ar->arvifs, list) {
		wdev = ieee80211_vif_to_wdev(arvif->ahvif->vif);
		if (!wdev) {
			ath12k_err(ar->ab, "Failed to find wdev\n");
			return -ENODATA;
		}
		ath12k_vendor_event_iface_reload_link(wiphy, wdev, arvif->link_id);
	}
	return 0;
}

/* From QSDK 14.0 ath12k_vendor_set_fixed_rate(), EHT MCS mode: fix the TX
 * rate of the vdev to an EHT MCS, with the maximum NSS of the radio
 */
static int ath12k_vendor_set_fixed_eht_mcs(struct ath12k_link_vif *arvif,
					   u32 value)
{
	struct ath12k *ar = arvif->ar;
	struct ieee80211_bss_conf *bss_conf;
	u32 rate_code;
	u8 nss;
	int ret;

	/* Allow pre-config before vdev is brought up. */
	if (arvif->is_up) {
		bss_conf = ath12k_mac_get_link_bss_conf(arvif);
		if (bss_conf && !bss_conf->eht_support) {
			ath12k_err(ar->ab, "EHT fixed rate is not allowed in current mode\n");
			return -EINVAL;
		}
	}

	if (value > ATH12K_EHT_MCS_MAX) {
		ath12k_warn(ar->ab, "EHT MCS %u exceeds max %u, clamping\n",
			    value, ATH12K_EHT_MCS_MAX);
		value = ATH12K_EHT_MCS_MAX;
	}

	nss = hweight32(ar->pdev->cap.tx_chain_mask);
	if (!nss) {
		ath12k_err(ar->ab, "invalid tx chain capability for nss\n");
		return -EINVAL;
	}

	rate_code = ATH12K_HW_RATE_CODE(value, nss - 1, WMI_RATE_PREAMBLE_EHT);
	ret = ath12k_wmi_vdev_set_param_cmd(ar, arvif->vdev_id,
					    WMI_VDEV_PARAM_FIXED_RATE, rate_code);
	if (ret) {
		ath12k_err(ar->ab, "Failed to send WMI_VDEV_PARAM_FIXED_RATE to firmware\n");
		return ret;
	}

	arvif->fixed_eht_mcs = value;
	arvif->fixed_eht_mcs_set = true;

	return 0;
}

/* The vendor parameters that are bits of the HE or EHT MU mode */
static const struct ath12k_vendor_mu_mode_param {
	u32 param;
	bool eht;
	u32 bits;
} ath12k_vendor_mu_mode_params[] = {
	{ QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_OFDMA, false, HE_MODE_UL_OFDMA },
	{ QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_OFDMA, true, EHT_MODE_UL_OFDMA },
	{ QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_MUMIMO, false, HE_MODE_UL_MUMIMO },
	{ QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_MUMIMO, true, EHT_MODE_MUMIMO },
	/* DL MU-MIMO is sent by the MU beamformer */
	{ QCA_WLAN_VENDOR_VDEV_PARAM_HE_DL_MUMIMO, false, HE_MODE_MU_TX_BFER },
	{ QCA_WLAN_VENDOR_VDEV_PARAM_EHT_DL_MUMIMO, true,
	  EHT_MODE_MU_TX_BFER | EHT_MODE_DL_OFDMA_MUMIMO },
};

static const struct ath12k_vendor_mu_mode_param *
ath12k_vendor_find_mu_mode_param(u32 param)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ath12k_vendor_mu_mode_params); i++)
		if (ath12k_vendor_mu_mode_params[i].param == param)
			return &ath12k_vendor_mu_mode_params[i];

	return NULL;
}

/* Force MU mode bits on (value 1) or off (value 0). If the MU mode was
 * already sent to the firmware it is sent again at once, else the bits are
 * applied when it is sent. This driver sends the EHT MU mode on AP
 * interfaces only.
 */
static int ath12k_vendor_set_mu_mode(struct ath12k_link_vif *arvif,
				     const struct ath12k_vendor_mu_mode_param *mp,
				     u32 value)
{
	struct ath12k *ar = arvif->ar;
	u32 *on = mp->eht ? &arvif->eht_mu_mode_on : &arvif->he_mu_mode_on;
	u32 *off = mp->eht ? &arvif->eht_mu_mode_off : &arvif->he_mu_mode_off;
	u32 *mode = mp->eht ? &arvif->eht_mu_mode : &arvif->he_mu_mode;
	bool sent = mp->eht ? arvif->eht_mu_mode_sent : arvif->he_mu_mode_sent;
	u32 new_on, new_off, mu_mode;
	int ret;

	if (value > 1) {
		ath12k_err(ar->ab, "MU mode value should be 0 or 1\n");
		return -EINVAL;
	}

	if (mp->eht && arvif->ahvif->vdev_type != WMI_VDEV_TYPE_AP) {
		ath12k_err(ar->ab, "EHT MU mode can be set on AP interfaces only\n");
		return -EOPNOTSUPP;
	}

	new_on = value ? *on | mp->bits : *on & ~mp->bits;
	new_off = value ? *off & ~mp->bits : *off | mp->bits;

	if (sent && arvif->is_up) {
		mu_mode = (*mode | new_on) & ~new_off;
		ret = ath12k_wmi_vdev_set_param_cmd(ar, arvif->vdev_id,
						    mp->eht ? WMI_VDEV_PARAM_SET_EHT_MU_MODE :
							      WMI_VDEV_PARAM_SET_HEMU_MODE,
						    mu_mode);
		if (ret) {
			ath12k_err(ar->ab, "Failed to send the MU mode to firmware\n");
			return ret;
		}
		*mode = mu_mode;
	}

	*on = new_on;
	*off = new_off;

	return 0;
}

/* 1 if all the bits are set in the MU mode last sent to the firmware,
 * with the forced bits applied
 */
static int ath12k_vendor_get_mu_mode(struct ath12k_link_vif *arvif,
				     const struct ath12k_vendor_mu_mode_param *mp,
				     u32 *value)
{
	u32 mu_mode;

	if (mp->eht && arvif->ahvif->vdev_type != WMI_VDEV_TYPE_AP)
		return -EOPNOTSUPP;

	if (!(mp->eht ? arvif->eht_mu_mode_sent : arvif->he_mu_mode_sent))
		return -ENODATA;

	if (mp->eht)
		mu_mode = (arvif->eht_mu_mode | arvif->eht_mu_mode_on) &
			  ~arvif->eht_mu_mode_off;
	else
		mu_mode = (arvif->he_mu_mode | arvif->he_mu_mode_on) &
			  ~arvif->he_mu_mode_off;

	*value = (mu_mode & mp->bits) == mp->bits;

	return 0;
}

/* Set one best effort EDCA parameter of the interface. mac80211 sets all
 * of them again when the AP or the BSS changes them.
 */
static int ath12k_vendor_set_be_wmm(struct ath12k_link_vif *arvif,
				    u32 param, u32 value)
{
	struct ath12k *ar = arvif->ar;
	struct wmi_wmm_params_arg old = arvif->wmm_params.ac_be;
	struct wmi_wmm_params_arg *be = &arvif->wmm_params.ac_be;
	int ret;

	/* all access categories are set once the interface is up */
	if (!arvif->is_up) {
		ath12k_err(ar->ab, "EDCA parameters can be set once the interface is up\n");
		return -ENOTCONN;
	}

	switch (param) {
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMIN:
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMAX:
		/* 2^n - 1 */
		if (value > 32767 || (value & (value + 1))) {
			ath12k_err(ar->ab, "CW should be 2^n - 1, up to 32767\n");
			return -EINVAL;
		}
		if (param == QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMIN)
			be->cwmin = value;
		else
			be->cwmax = value;
		if (be->cwmin > be->cwmax) {
			ath12k_err(ar->ab, "CW min %u is above CW max %u\n",
				   be->cwmin, be->cwmax);
			*be = old;
			return -EINVAL;
		}
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_AIFS:
		if (value < 1 || value > 15) {
			ath12k_err(ar->ab, "AIFSN should be between 1 and 15\n");
			return -EINVAL;
		}
		be->aifs = value;
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_TXOP:
		if (value > U16_MAX) {
			ath12k_err(ar->ab, "TXOP limit should be up to 65535 (x 32 us)\n");
			return -EINVAL;
		}
		be->txop = value;
		break;
	default:
		return -EINVAL;
	}

	ret = ath12k_wmi_send_wmm_update_cmd(ar, arvif->vdev_id,
					     &arvif->wmm_params);
	if (ret) {
		ath12k_err(ar->ab, "Failed to send the WMM parameters to firmware\n");
		*be = old;
	}

	return ret;
}

static int ath12k_vendor_get_be_wmm(struct ath12k_link_vif *arvif,
				    u32 param, u32 *value)
{
	struct wmi_wmm_params_arg *be = &arvif->wmm_params.ac_be;

	switch (param) {
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMIN:
		*value = be->cwmin;
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMAX:
		*value = be->cwmax;
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_AIFS:
		*value = be->aifs;
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_TXOP:
		*value = be->txop;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* Broadcast or restricted TWT on or off. Applied at once if TWT is
 * enabled, else when it is.
 */
static int ath12k_vendor_set_twt(struct ath12k *ar, u32 param, u32 value)
{
	bool *cfg = param == QCA_WLAN_VENDOR_RADIO_PARAM_TWT_BCAST ?
		    &ar->twt_bcast : &ar->twt_rtwt;
	bool old = *cfg;
	int ret;

	if (value > 1) {
		ath12k_err(ar->ab, "TWT value should be 0 or 1\n");
		return -EINVAL;
	}

	*cfg = value;
	ret = ath12k_mac_twt_reconfig(ar);
	if (ret) {
		ath12k_err(ar->ab, "Failed to send the TWT configuration to firmware\n");
		*cfg = old;
	}

	return ret;
}

/* A vendor parameter that is one WMI vdev or pdev parameter. As in the
 * QSDK 14.0 setters, the value is checked and sent as is. The meaning of
 * the values is in the firmware's wmi_unified.h.
 *
 * The firmware cannot be asked for these parameters, so the getters return
 * the value last sent to it: every write of the WMI parameter is recorded,
 * also the driver's own (RTS/CTS, protection and BA mode, NSS, EHT LTF,
 * HE sounding mode, station kickout threshold, ARP access category). A
 * parameter never sent has the firmware's default, which the driver does
 * not know; its getter fails with -ENODATA.
 */
struct ath12k_vendor_wmi_param {
	u32 param;
	u32 wmi_param;
	s64 min;
	s64 max;
	/* the value is signed */
	bool sign;
	/* format bits of the WMI value: the UL trigger parameters tell HE (0)
	 * from EHT (WMI_HOST_VDEV_PARAM_EHT_MODE) in bits 28-31
	 */
	u32 fmt_mask;
	u32 fmt;
	/* further checks, may change the value */
	int (*check)(struct ath12k *ar, u32 *value);
};

#define ATH12K_VENDOR_VDEV_WMI(_param, _wmi, _min, _max, ...)		\
	{ .param = QCA_WLAN_VENDOR_VDEV_PARAM_ ## _param,		\
	  .wmi_param = WMI_VDEV_PARAM_ ## _wmi,				\
	  .min = (_min), .max = (_max), __VA_ARGS__ }

#define ATH12K_VENDOR_PDEV_WMI(_param, _wmi, _min, _max, ...)		\
	{ .param = QCA_WLAN_VENDOR_RADIO_PARAM_ ## _param,		\
	  .wmi_param = WMI_PDEV_PARAM_ ## _wmi,				\
	  .min = (_min), .max = (_max), __VA_ARGS__ }

#define ATH12K_VENDOR_UL_FMT_MASK GENMASK(31, 28)

/* BA mode 4 is a placeholder in the firmware, not a valid mode */
static int ath12k_vendor_check_ba_mode(struct ath12k *ar, u32 *value)
{
	if (*value == 4) {
		ath12k_err(ar->ab, "BA mode 4 is not valid\n");
		return -EINVAL;
	}

	return 0;
}

/* Bits 24-31 bandwidth (0-6), bits 16-23 NSS, bits 0-15 rate mask */
static int ath12k_vendor_check_bw_nss_rate(struct ath12k *ar, u32 *value)
{
	u32 bw = u32_get_bits(*value, GENMASK(31, 24));
	u32 nss = u32_get_bits(*value, GENMASK(23, 16));
	u32 ratemask = u32_get_bits(*value, GENMASK(15, 0));

	if (bw > 6 ||
	    (bw >= 3 && ratemask > 0x3ff) ||
	    (bw >= 1 && bw <= 2 && ratemask > 0xff) ||
	    (bw >= 1 && (nss == 0 || nss > 4))) {
		ath12k_err(ar->ab, "Invalid bandwidth, NSS or rate mask: 0x%x\n",
			   *value);
		return -EINVAL;
	}

	return 0;
}

/* Bits 0-3 RTS/CTS flag (0-2), bits 4-7 profile (0-4). The profile is 0
 * if and only if the flag is 0.
 */
static int ath12k_vendor_check_rtscts(struct ath12k *ar, u32 *value)
{
	u32 flag = u32_get_bits(*value, GENMASK(3, 0));
	u32 profile = u32_get_bits(*value, GENMASK(7, 4));

	if (!flag != !profile || flag > 2 || profile > 4) {
		ath12k_err(ar->ab, "Invalid RTS/CTS value: 0x%x\n", *value);
		return -EINVAL;
	}

	return 0;
}

/* UL trigger GI: 0 is 0.8 us, 2 is 1.6 us, 3 is 3.2 us. 1 (0.4 us) is not
 * an HE or EHT GI.
 */
static int ath12k_vendor_check_ul_gi(struct ath12k *ar, u32 *value)
{
	if (*value == 1) {
		ath12k_err(ar->ab, "UL GI should be 0, 2 or 3\n");
		return -EINVAL;
	}

	return 0;
}

/* NSS 0 means the number of TX chains of the radio */
static int ath12k_vendor_check_nss(struct ath12k *ar, u32 *value)
{
	u32 max_nss = hweight32(ar->pdev->cap.tx_chain_mask);

	if (!*value)
		*value = max_nss;

	if (!max_nss || *value > max_nss) {
		ath12k_err(ar->ab, "NSS should be 1 to %u\n", max_nss);
		return -EINVAL;
	}

	return 0;
}

#define ATH12K_VENDOR_ACK_TIMEOUT_MIN 0x40
#define ATH12K_VENDOR_ACK_TIMEOUT_MAX 0xff

/* RX ACK timeout in bits 0-15; pre-RX ACK timeout in bits 16-31, the RX
 * ACK timeout - 20 if out of range
 */
static int ath12k_vendor_check_ack_timeout(struct ath12k *ar, u32 *value)
{
	u32 rx = *value & 0xffff;
	u32 pre_rx = *value >> 16;

	if (rx < ATH12K_VENDOR_ACK_TIMEOUT_MIN ||
	    rx > ATH12K_VENDOR_ACK_TIMEOUT_MAX) {
		ath12k_err(ar->ab, "TX ACK timeout should be 0x40 to 0xff\n");
		return -EINVAL;
	}

	if (pre_rx < ATH12K_VENDOR_ACK_TIMEOUT_MIN ||
	    pre_rx > ATH12K_VENDOR_ACK_TIMEOUT_MAX)
		pre_rx = rx - 20;

	*value = rx | (pre_rx << 16);

	return 0;
}

static const struct ath12k_vendor_wmi_param ath12k_vendor_vdev_wmi_params[] = {
	ATH12K_VENDOR_VDEV_WMI(DYN_BW_RTS, DISABLE_DYN_BW_RTS, 0, U32_MAX),
	/* bits 0-2: rate drop down of management, SU data and MU data */
	ATH12K_VENDOR_VDEV_WMI(RATE_DROPDOWN, RATE_DROPDOWN_BMAP, 0, 7),
	ATH12K_VENDOR_VDEV_WMI(RC_NUM_RETRIES, RC_NUM_RETRIES, 0, U32_MAX),
	ATH12K_VENDOR_VDEV_WMI(HE_SOUNDING_MODE, SET_HE_SOUNDING_MODE,
			       0, U32_MAX),
	ATH12K_VENDOR_VDEV_WMI(GTX_ENABLE, GTX_ENABLE, 0, U32_MAX),
	ATH12K_VENDOR_VDEV_WMI(HWCTS2SELF_OFDMA, HWCTS2SELF_OFDMA, 0, 1),
	/* a BA mode: 0 default, 1 manual, 2, 3, 5, 6 BA window 64, 256,
	 * 512, 1024
	 */
	ATH12K_VENDOR_VDEV_WMI(BA_BUFSIZE, BA_MODE, 0, 6,
			       .check = ath12k_vendor_check_ba_mode),
	ATH12K_VENDOR_VDEV_WMI(PROTECTION_MODE, PROTECTION_MODE, 0, U32_MAX),
	ATH12K_VENDOR_VDEV_WMI(BW_NSS_RATE, BW_NSS_RATEMASK, 0, U32_MAX,
			       .check = ath12k_vendor_check_bw_nss_rate),
	/* UL trigger: LDPC, LTF, max NSS, PPDU bandwidth, GI, STBC */
	ATH12K_VENDOR_VDEV_WMI(EHT_UL_LDPC, UL_LDPC, 0, 1,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK,
			       .fmt = WMI_HOST_VDEV_PARAM_EHT_MODE),
	ATH12K_VENDOR_VDEV_WMI(EHT_UL_LTF, UL_EHT_LTF, 0, 3),
	ATH12K_VENDOR_VDEV_WMI(EHT_UL_NSS, UL_NSS, 0, 0xfffffff,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK,
			       .fmt = WMI_HOST_VDEV_PARAM_EHT_MODE),
	ATH12K_VENDOR_VDEV_WMI(EHT_UL_PPDU_BW, UL_PPDU_BW, 0, 4,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK,
			       .fmt = WMI_HOST_VDEV_PARAM_EHT_MODE),
	ATH12K_VENDOR_VDEV_WMI(EHT_UL_SHORTGI, UL_GI, 0, 3,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK,
			       .fmt = WMI_HOST_VDEV_PARAM_EHT_MODE,
			       .check = ath12k_vendor_check_ul_gi),
	ATH12K_VENDOR_VDEV_WMI(EHT_UL_STBC, UL_STBC, 0, 1,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK,
			       .fmt = WMI_HOST_VDEV_PARAM_EHT_MODE),
	ATH12K_VENDOR_VDEV_WMI(ENABLERTSCTS, ENABLE_RTSCTS, 0, 0xff,
			       .check = ath12k_vendor_check_rtscts),
	ATH12K_VENDOR_VDEV_WMI(HE_DCM, HE_DCM, 0, 1),
	ATH12K_VENDOR_VDEV_WMI(HE_EXTRANGE, HE_RANGE_EXT, 0, 1),
	ATH12K_VENDOR_VDEV_WMI(HE_UL_LDPC, UL_LDPC, 0, 1,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK),
	ATH12K_VENDOR_VDEV_WMI(HE_UL_NSS, UL_NSS, 0, 0xfffffff,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK),
	ATH12K_VENDOR_VDEV_WMI(HE_UL_PPDU_BW, UL_PPDU_BW, 0, 3,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK),
	ATH12K_VENDOR_VDEV_WMI(HE_UL_SHORTGI, UL_GI, 0, 3,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK,
			       .check = ath12k_vendor_check_ul_gi),
	ATH12K_VENDOR_VDEV_WMI(HE_UL_STBC, UL_STBC, 0, 1,
			       .fmt_mask = ATH12K_VENDOR_UL_FMT_MASK),
	/* rate code of RTS and CTS */
	ATH12K_VENDOR_VDEV_WMI(RTSCTS_RATE, RTSCTS_RATE, 0, U32_MAX),
	ATH12K_VENDOR_VDEV_WMI(DIS_LPI_ANT_OPTIMIZE,
			       DISABLE_LPI_ANT_OPTIMIZATION, 0, U32_MAX),
	ATH12K_VENDOR_VDEV_WMI(NSS, NSS, 0, U32_MAX,
			       .check = ath12k_vendor_check_nss),
	/* enum wmi_channel_width */
	ATH12K_VENDOR_VDEV_WMI(CHWIDTH, CHWIDTH, 0, 0xf),
	ATH12K_VENDOR_VDEV_WMI(EHT_LTF, EHT_LTF, 0, 3),
};

static const struct ath12k_vendor_wmi_param ath12k_vendor_pdev_wmi_params[] = {
	ATH12K_VENDOR_PDEV_WMI(MGMT_RETRY_LIMIT, MGMT_RETRY_LIMIT, 1, 14),
	ATH12K_VENDOR_PDEV_WMI(RTS_CTS_RATE, RTS_FIXED_RATE, 0, 4),
	/* retry and station kickout thresholds, 0 disables */
	ATH12K_VENDOR_PDEV_WMI(NON_AGG_SW_RETRY_TH, NON_AGG_SW_RETRY_TH,
			       0, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(AGG_SW_RETRY_TH, AGG_SW_RETRY_TH, 0, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(STA_KICKOUT_TH, STA_KICKOUT_TH, 0, U32_MAX),
	/* access category of ARP and DHCP: 0 BE, 1 BK, 2 VI, 3 VO */
	ATH12K_VENDOR_PDEV_WMI(ARPDHCP_AC_OVERRIDE, ARP_AC_OVERRIDE, 0, 3),
	ATH12K_VENDOR_PDEV_WMI(ANI_ENABLE, ANI_ENABLE, 0, 1),
	ATH12K_VENDOR_PDEV_WMI(ANI_POLL_PERIOD, ANI_POLL_PERIOD, 1, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(ANI_LISTEN_PERIOD, ANI_LISTEN_PERIOD,
			       1, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(ANI_OFDM_LEVEL, ANI_OFDM_LEVEL, -128, 127,
			       .sign = true),
	/* dBm. QSDK clamps the value to this range. */
	ATH12K_VENDOR_PDEV_WMI(CCA_THRESHOLD, CCA_THRESHOLD, -94, -11,
			       .sign = true),
	/* dBm */
	ATH12K_VENDOR_PDEV_WMI(SENS_LEVEL, SENSITIVITY_LEVEL, 0, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(DYN_GROUPING, MU_GROUP_POLICY, 0, 1),
	ATH12K_VENDOR_PDEV_WMI(BURST_DUR, BURST_DUR, 0, 8192),
	ATH12K_VENDOR_PDEV_WMI(BURST_ENABLE, BURST_ENABLE, 0, 1),
	ATH12K_VENDOR_PDEV_WMI(DISABLE_LPI_ANT, DISABLE_LPI_ANT_OPTIMIZATION,
			       0, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(UL_OFDMA_RTD, UL_OFDMA_RTD, 0, 600),
	ATH12K_VENDOR_PDEV_WMI(ENABLE_SMALL_MRU, ENABLE_SMALL_MRU, 0, 1),
	ATH12K_VENDOR_PDEV_WMI(ENABLE_LARGE_MRU, ENABLE_LARGE_MRU, 0, 1),
	/* us */
	ATH12K_VENDOR_PDEV_WMI(SET_PPDU_DURATION_CMDID, SET_PPDU_DURATION_CMDID,
			       0, U32_MAX),
	/* ms */
	ATH12K_VENDOR_PDEV_WMI(TXBF_SOUND_PERIOD_CMDID, TXBF_SOUND_PERIOD_CMDID,
			       10, 10000),
	/* 0 data-CTS, 1 data-data, 2 data-data or CTS */
	ATH12K_VENDOR_PDEV_WMI(SET_BURST_MODE_CMDID, SET_BURST_MODE_CMDID,
			       0, 2),
	/* bitmaps of the TIDs */
	ATH12K_VENDOR_PDEV_WMI(ENABLE_AMSDU, ENABLE_PER_TID_AMSDU, 0, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(ENABLE_AMPDU, ENABLE_PER_TID_AMPDU, 0, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(CTS_TIMEOUT, CTS_TIMEOUT, 1, 0xff),
	ATH12K_VENDOR_PDEV_WMI(SLOT_TIME, SLOT_TIME, 1, 90),
	ATH12K_VENDOR_PDEV_WMI(ACK_TIMEOUT, ACK_TIMEOUT, 0, U32_MAX,
			       .check = ath12k_vendor_check_ack_timeout),
	/* 1: equal RU allocation for DL and UL OFDMA, 0: dynamic */
	ATH12K_VENDOR_PDEV_WMI(EQUAL_RU_ALLOCATION_ENABLE,
			       EQUAL_RU_ALLOCATION_ENABLE, 0, 1),
	/* DL, us */
	ATH12K_VENDOR_PDEV_WMI(SET_MU_PPDU_DURATION, SET_MU_PPDU_DURATION,
			       0, U32_MAX),
	/* bitmap of the punctured 20 MHz subchannels */
	ATH12K_VENDOR_PDEV_WMI(SET_PREAM_PUNCT_BW, SET_PREAM_PUNCT_BW,
			       0, U32_MAX),
	ATH12K_VENDOR_PDEV_WMI(LOW_LATENCY_SCHED_MODE, LOW_LATENCY_SCHED_MODE,
			       0, U32_MAX),
	/* ms */
	ATH12K_VENDOR_PDEV_WMI(MSDU_TTL, SET_MSDU_TTL_CMDID, 0, U32_MAX),
};

static_assert(ARRAY_SIZE(ath12k_vendor_vdev_wmi_params) <=
	      ATH12K_VENDOR_WMI_PARAMS_MAX);
static_assert(ARRAY_SIZE(ath12k_vendor_pdev_wmi_params) <=
	      ATH12K_VENDOR_WMI_PARAMS_MAX);

static int ath12k_vendor_find_wmi_param(const struct ath12k_vendor_wmi_param *wps,
					int n, u32 param)
{
	int i;

	for (i = 0; i < n; i++)
		if (wps[i].param == param)
			return i;

	return -ENOENT;
}

/* Record a WMI parameter sent to the firmware, by anyone */
static void ath12k_vendor_wmi_param_sent(const struct ath12k_vendor_wmi_param *wps,
					 int n, struct ath12k_vendor_wmi_vals *vals,
					 u32 wmi_param, u32 value)
{
	int i;

	for (i = 0; i < n; i++) {
		if (wps[i].wmi_param != wmi_param ||
		    (value & wps[i].fmt_mask) != wps[i].fmt)
			continue;

		WRITE_ONCE(vals->val[i], value & ~wps[i].fmt_mask);
		/* the value before the bit, for ath12k_vendor_get_wmi_val() */
		smp_mb__before_atomic();
		set_bit(i, vals->sent);
	}
}

void ath12k_vendor_vdev_param_sent(struct ath12k *ar, u32 vdev_id,
				   u32 wmi_param, u32 value)
{
	if (vdev_id >= ATH12K_VENDOR_MAX_VDEVS)
		return;

	ath12k_vendor_wmi_param_sent(ath12k_vendor_vdev_wmi_params,
				     ARRAY_SIZE(ath12k_vendor_vdev_wmi_params),
				     &ar->vendor_vdev_wmi[vdev_id],
				     wmi_param, value);
}

void ath12k_vendor_pdev_param_sent(struct ath12k *ar, u32 wmi_param,
				   u32 value)
{
	ath12k_vendor_wmi_param_sent(ath12k_vendor_pdev_wmi_params,
				     ARRAY_SIZE(ath12k_vendor_pdev_wmi_params),
				     &ar->vendor_pdev_wmi, wmi_param, value);
}

static int ath12k_vendor_get_wmi_val(struct ath12k_vendor_wmi_vals *vals,
				     int idx, u32 *value)
{
	if (!test_bit(idx, vals->sent))
		return -ENODATA;

	/* the bit before the value, see ath12k_vendor_wmi_param_sent() */
	smp_rmb();
	*value = READ_ONCE(vals->val[idx]);

	return 0;
}

static int ath12k_vendor_check_wmi_param(struct ath12k *ar,
					 const struct ath12k_vendor_wmi_param *wp,
					 u32 *value)
{
	s64 v = wp->sign ? (s64)(s32)*value : (s64)*value;

	if (v < wp->min || v > wp->max) {
		ath12k_err(ar->ab, "Value %lld should be %lld to %lld\n",
			   v, wp->min, wp->max);
		return -EINVAL;
	}

	return wp->check ? wp->check(ar, value) : 0;
}

static int ath12k_vendor_set_vdev_wmi_param(struct ath12k_link_vif *arvif,
					    int idx, u32 value)
{
	const struct ath12k_vendor_wmi_param *wp =
		&ath12k_vendor_vdev_wmi_params[idx];
	struct ath12k *ar = arvif->ar;
	int ret;

	if (!arvif->is_created) {
		ath12k_err(ar->ab, "The interface has no firmware vdev yet\n");
		return -ENOTCONN;
	}

	ret = ath12k_vendor_check_wmi_param(ar, wp, &value);
	if (ret)
		return ret;

	ret = ath12k_wmi_vdev_set_param_cmd(ar, arvif->vdev_id, wp->wmi_param,
					    value | wp->fmt);
	if (ret)
		ath12k_err(ar->ab, "Failed to send vdev param %u to firmware\n",
			   wp->wmi_param);

	return ret;
}

static int ath12k_vendor_get_vdev_wmi_param(struct ath12k_link_vif *arvif,
					    int idx, u32 *value)
{
	if (!arvif->is_created || !arvif->ar ||
	    arvif->vdev_id >= ATH12K_VENDOR_MAX_VDEVS)
		return -ENODATA;

	return ath12k_vendor_get_wmi_val(&arvif->ar->vendor_vdev_wmi[arvif->vdev_id],
					 idx, value);
}

#define ATH12K_VENDOR_DEFAULT_BURST_DUR 8160

static int ath12k_vendor_set_pdev_wmi_param(struct ath12k *ar, int idx,
					    u32 value)
{
	const struct ath12k_vendor_wmi_param *wp =
		&ath12k_vendor_pdev_wmi_params[idx];
	int dur_idx;
	u32 dur;
	int ret;

	ret = ath12k_vendor_check_wmi_param(ar, wp, &value);
	if (ret)
		return ret;

	ret = ath12k_wmi_pdev_set_param(ar, wp->wmi_param, value,
					ar->pdev->pdev_id);
	if (ret) {
		ath12k_err(ar->ab, "Failed to send pdev param %u to firmware\n",
			   wp->wmi_param);
		return ret;
	}

	/* As QSDK: bursting gets a duration if none was set */
	if (wp->param == QCA_WLAN_VENDOR_RADIO_PARAM_BURST_ENABLE) {
		dur_idx = ath12k_vendor_find_wmi_param(ath12k_vendor_pdev_wmi_params,
						       ARRAY_SIZE(ath12k_vendor_pdev_wmi_params),
						       QCA_WLAN_VENDOR_RADIO_PARAM_BURST_DUR);
		if (ath12k_vendor_get_wmi_val(&ar->vendor_pdev_wmi, dur_idx, &dur) ||
		    !dur)
			ret = ath12k_vendor_set_pdev_wmi_param(ar, dur_idx,
							       ATH12K_VENDOR_DEFAULT_BURST_DUR);
	}

	return ret;
}

/* Set link-vif level parameters
 * set 'reload' to true to send reload event to userspace
 */
static int ath12k_vendor_set_arvif_params(struct ath12k_link_vif *arvif,
					  u32 param, u32 value, bool *reload)
{
	struct ath12k *ar = arvif->ar;
	int ret = -1;
	int idx;

	switch (param) {
	case QCA_WLAN_VENDOR_VDEV_PARAM_TEST_RELOAD:
		*reload = true;
		ret = 0;
		break;
	case PARAM_RADIO_TXCHAINSOFT:
		ret = ath12k_mac_set_tx_antenna(ar, value);
		break;
	case ACFG_PARAM_RADIO_RXCHAINMASK:
		ret = ath12k_mac_set_rx_antenna(ar, value);
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_FIXED_EHT_MCS:
		ret = ath12k_vendor_set_fixed_eht_mcs(arvif, value);
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_OFDMA:
	case QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_OFDMA:
	case QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_MUMIMO:
	case QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_MUMIMO:
	case QCA_WLAN_VENDOR_VDEV_PARAM_HE_DL_MUMIMO:
	case QCA_WLAN_VENDOR_VDEV_PARAM_EHT_DL_MUMIMO:
		ret = ath12k_vendor_set_mu_mode(arvif,
						ath12k_vendor_find_mu_mode_param(param),
						value);
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMIN:
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMAX:
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_AIFS:
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_TXOP:
		ret = ath12k_vendor_set_be_wmm(arvif, param, value);
		break;
	default:
		idx = ath12k_vendor_find_wmi_param(ath12k_vendor_vdev_wmi_params,
						   ARRAY_SIZE(ath12k_vendor_vdev_wmi_params),
						   param);
		if (idx >= 0) {
			ret = ath12k_vendor_set_vdev_wmi_param(arvif, idx, value);
			break;
		}
		ath12k_dbg(ar->ab, ATH12K_DBG_CFG,
			   "Un-supported param: %d\n", param);
		break;
	}

	return ret;
}

/* Set radio level parameters
 * set 'reload' to true to send reload event to userspace
 */
static int ath12k_vendor_set_radio_params(struct ath12k *ar,
					  u32 param, u32 value, bool *reload)
{
	int ret = -1;
	int idx;

	switch (param) {
	case QCA_WLAN_VENDOR_RADIO_PARAM_TEST_RELOAD:
		*reload = true;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_TWT_BCAST:
	case QCA_WLAN_VENDOR_RADIO_PARAM_TWT_RTWT:
		ret = ath12k_vendor_set_twt(ar, param, value);
		break;
	case PARAM_RADIO_TXCHAINSOFT:
		ret = ath12k_mac_set_tx_antenna(ar, value);
		break;
	case ACFG_PARAM_RADIO_RXCHAINMASK:
		ret = ath12k_mac_set_rx_antenna(ar, value);
		break;
	default:
		idx = ath12k_vendor_find_wmi_param(ath12k_vendor_pdev_wmi_params,
						   ARRAY_SIZE(ath12k_vendor_pdev_wmi_params),
						   param);
		if (idx >= 0) {
			ret = ath12k_vendor_set_pdev_wmi_param(ar, idx, value);
			break;
		}
		ath12k_dbg(ar->ab, ATH12K_DBG_CFG,
			   "Un-supported param: %d\n", param);
		break;
	}

	return ret;
}

/* Find the link-vif of the wdev with the link id to set params
 * If link id is invalid use the default link
 */
static int ath12k_vendor_set_wifi_params(struct wiphy *wiphy,
					 struct wireless_dev *wdev,
					 struct ath12k_wifi_generic_params *params)
{
	struct ieee80211_vif *vif = NULL;
	struct ath12k_vif *ahvif;
	struct ath12k_link_vif *arvif = NULL;
	struct ath12k_hw *ah = NULL;
	u32 param = params->value;
	bool reload = false;
	u32 value;
	int ret = -1;

	lockdep_assert_wiphy(wiphy);

	vif = wdev_to_ieee80211_vif(wdev);
	if (!vif || params->data_len < sizeof(value))
		return -EINVAL;

	memcpy(&value, params->data, sizeof(value));

	ahvif = ath12k_vif_to_ahvif(vif);
	if (!ahvif)
		return -EINVAL;

	ah = ahvif->ah;

	rcu_read_lock();
	if (ah && params->link_id == INVALID_LINK_ID) {
		arvif = &ahvif->deflink;
	} else {
		if (params->link_id < ATH12K_NUM_MAX_LINKS)
			arvif = rcu_dereference(ahvif->link[params->link_id]);
	}
	if (!arvif || !arvif->ar) {
		rcu_read_unlock();
		return -EINVAL;
	}
	rcu_read_unlock();

	ath12k_dbg(NULL, ATH12K_DBG_CFG,
		   "vif: %p param: %d value: %d if: %d link: %d\n",
		   vif, param, value,
		   params->ifindex, params->link_id);

	ret = ath12k_vendor_set_arvif_params(arvif, param, value, &reload);

	if (!ret && reload)
		ath12k_vendor_event_iface_reload_link(wiphy, wdev, params->link_id);

	return ret;
}

/* Find the 'ar' radio instance using the radio index of the wiphy */
static struct ath12k *ath12k_vendor_get_radio(struct wiphy *wiphy,
					      u8 radio_idx)
{
	struct ath12k_hw *ah = ath12k_hw_to_ah(wiphy_to_ieee80211_hw(wiphy));

	if (radio_idx == INVALID_RADIO_INDEX || radio_idx >= ah->num_radio)
		return NULL;

	return ath12k_ah_to_ar(ah, radio_idx);
}

/* Find the 'ar' radio instance using the radio index of the wiphy
 * to set params
 */
static int ath12k_vendor_set_wiphy_params(struct wiphy *wiphy,
					  struct ath12k_wifi_generic_params *params)
{
	struct ath12k *ar = NULL;
	u32 param = params->value;
	u32 value;
	int ret = -1;
	bool reload = false;

	lockdep_assert_wiphy(wiphy);

	if (params->data_len < sizeof(value))
		return -EINVAL;

	memcpy(&value, params->data, sizeof(value));

	ar = ath12k_vendor_get_radio(wiphy, params->radio_idx);
	if (!ar)
		return -ENODATA;

	ath12k_dbg(ar->ab, ATH12K_DBG_CFG,
		   "ar: %p param: %d value: %d if: %d radio: %d\n",
		   ar, param, value,
		   params->ifindex, params->radio_idx);

	ret = ath12k_vendor_set_radio_params(ar, param, value, &reload);

	if (!ret && reload)
		ath12k_vendor_event_iface_reload(wiphy, ar);

	return ret;
}

/* Get link-vif level parameters */
static int ath12k_vendor_get_arvif_params(struct ath12k_link_vif *arvif,
					  u32 param, u32 *value)
{
	int ret = -1;
	int idx;

	switch (param) {
	case PARAM_RADIO_TXCHAINSOFT:
		*value = arvif->ar->cfg_tx_chainmask;
		ret = 0;
		break;
	case ACFG_PARAM_RADIO_RXCHAINMASK:
		*value = arvif->ar->cfg_rx_chainmask;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_TEST:
		*value = 0;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_FIXED_EHT_MCS:
		if (!arvif->fixed_eht_mcs_set) {
			ret = -ENODATA;
			break;
		}
		*value = arvif->fixed_eht_mcs;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_OFDMA:
	case QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_OFDMA:
	case QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_MUMIMO:
	case QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_MUMIMO:
	case QCA_WLAN_VENDOR_VDEV_PARAM_HE_DL_MUMIMO:
	case QCA_WLAN_VENDOR_VDEV_PARAM_EHT_DL_MUMIMO:
		ret = ath12k_vendor_get_mu_mode(arvif,
						ath12k_vendor_find_mu_mode_param(param),
						value);
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMIN:
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMAX:
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_AIFS:
	case QCA_WLAN_VENDOR_VDEV_PARAM_BE_TXOP:
		ret = ath12k_vendor_get_be_wmm(arvif, param, value);
		break;
	default:
		idx = ath12k_vendor_find_wmi_param(ath12k_vendor_vdev_wmi_params,
						   ARRAY_SIZE(ath12k_vendor_vdev_wmi_params),
						   param);
		if (idx >= 0) {
			ret = ath12k_vendor_get_vdev_wmi_param(arvif, idx, value);
			break;
		}
		ath12k_dbg(NULL, ATH12K_DBG_CFG,
			   "Un-supported param: %d\n", param);
		break;
	}

	return ret;
}

/* Get radio level parameters */
static int ath12k_vendor_get_radio_params(struct ath12k *ar,
					  u32 param, u32 *value)
{
	int ret = -1;
	int idx;

	switch (param) {
	case QCA_WLAN_VENDOR_RADIO_PARAM_TEST:
		*value = 0;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_TWT_BCAST:
		*value = ar->twt_bcast;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_TWT_RTWT:
		*value = ar->twt_rtwt;
		ret = 0;
		break;
	case PARAM_RADIO_TXCHAINSOFT:
		*value = ar->cfg_tx_chainmask;
		ret = 0;
		break;
	case ACFG_PARAM_RADIO_RXCHAINMASK:
		*value = ar->cfg_rx_chainmask;
		ret = 0;
		break;
	default:
		idx = ath12k_vendor_find_wmi_param(ath12k_vendor_pdev_wmi_params,
						   ARRAY_SIZE(ath12k_vendor_pdev_wmi_params),
						   param);
		if (idx >= 0) {
			ret = ath12k_vendor_get_wmi_val(&ar->vendor_pdev_wmi, idx,
							value);
			break;
		}
		ath12k_dbg(ar->ab, ATH12K_DBG_CFG,
			   "Un-supported param: %d\n", param);
		break;
	}

	return ret;
}

/* Find the link-vif of the wdev with the link id to get params
 * If link id is invalid use the default link
 */
static int ath12k_vendor_get_wifi_params(struct wiphy *wiphy,
					 struct wireless_dev *wdev,
					 struct ath12k_wifi_generic_params *params,
					 u32 *value)
{
	struct ieee80211_vif *vif = NULL;
	struct ath12k_vif *ahvif;
	struct ath12k_link_vif *arvif = NULL;
	struct ath12k_hw *ah;
	int param = params->value;
	int ret = -1;

	vif = wdev_to_ieee80211_vif(wdev);
	if (!vif)
		return -EINVAL;

	ahvif = ath12k_vif_to_ahvif(vif);
	if (!ahvif)
		return -EINVAL;

	ah = ahvif->ah;

	rcu_read_lock();
	if (ah && params->link_id == INVALID_LINK_ID) {
		arvif = &ahvif->deflink;
	} else {
		if (params->link_id < ATH12K_NUM_MAX_LINKS)
			arvif = rcu_dereference(ahvif->link[params->link_id]);
	}
	if (!arvif) {
		rcu_read_unlock();
		return -EINVAL;
	}
	ath12k_dbg(NULL, ATH12K_DBG_CFG,
		   "vif: %p param: %d if: %d link: %d\n",
		   vif, param, params->ifindex, params->link_id);

	ret = ath12k_vendor_get_arvif_params(arvif, param, value);
	rcu_read_unlock();

	return ret;
}

/* Find the 'ar' radio instance using the radio index of the wiphy
 * to get params
 */
static int ath12k_vendor_get_wiphy_params(struct wiphy *wiphy,
					  struct ath12k_wifi_generic_params *params,
					  u32 *value)
{
	struct ath12k *ar = NULL;
	int param = params->value;
	int ret = -1;

	ar = ath12k_vendor_get_radio(wiphy, params->radio_idx);
	if (!ar)
		return -ENODATA;

	ath12k_dbg(ar->ab, ATH12K_DBG_CFG,
		   "ar: %p param: %d if: %d radio: %d\n",
		   ar, param, params->ifindex, params->radio_idx);
	ret = ath12k_vendor_get_radio_params(ar, param, value);

	return ret;
}

/* The link-vif of the wdev with the link id, the default link if the
 * link id is invalid
 */
static struct ath12k_link_vif *
ath12k_vendor_find_arvif(struct wiphy *wiphy, struct wireless_dev *wdev,
			 u8 link_id)
{
	struct ieee80211_vif *vif = wdev_to_ieee80211_vif(wdev);
	struct ath12k_link_vif *arvif;
	struct ath12k_vif *ahvif;

	if (!vif)
		return NULL;

	ahvif = ath12k_vif_to_ahvif(vif);
	if (link_id == INVALID_LINK_ID)
		arvif = &ahvif->deflink;
	else if (link_id < ATH12K_NUM_MAX_LINKS)
		arvif = wiphy_dereference(wiphy, ahvif->link[link_id]);
	else
		arvif = NULL;

	if (!arvif || !arvif->ar)
		return NULL;

	return arvif;
}

/* A station of the link-vif. It stays valid while the wiphy lock is held:
 * stations are removed under it.
 */
static struct ath12k_link_sta *
ath12k_vendor_find_arsta(struct ath12k_link_vif *arvif,
			 struct ath12k_wifi_generic_params *params)
{
	struct ath12k_base *ab = arvif->ar->ab;
	struct ath12k_link_sta *arsta;

	if (!params->data || params->data_len != ETH_ALEN)
		return NULL;

	spin_lock_bh(&ab->base_lock);
	arsta = ath12k_link_sta_find_by_addr(ab, params->data);
	spin_unlock_bh(&ab->base_lock);

	if (!arsta || arsta->arvif != arvif)
		return NULL;

	return arsta;
}

/* QSDK 14.0 set_sched_ofdma_enable: stop (0) or allow (1) OFDMA for one
 * station. QSDK sends peer param 26, which the firmware's wmi_unified.h
 * names WMI_PEER_PARAM_MU_ENABLE; this sends WMI_PEER_PARAM_OFDMA_ENABLE.
 */
static int ath12k_vendor_set_peer_ofdma(struct wiphy *wiphy,
					struct wireless_dev *wdev,
					struct ath12k_wifi_generic_params *params)
{
	struct ath12k_link_vif *arvif;
	struct ath12k_link_sta *arsta;
	int ret;

	lockdep_assert_wiphy(wiphy);

	arvif = ath12k_vendor_find_arvif(wiphy, wdev, params->link_id);
	if (!arvif)
		return -EINVAL;

	if (params->value > 1) {
		ath12k_err(arvif->ar->ab, "OFDMA enable value should be 0 or 1\n");
		return -EINVAL;
	}

	arsta = ath12k_vendor_find_arsta(arvif, params);
	if (!arsta) {
		ath12k_err(arvif->ar->ab, "No such station on the interface\n");
		return -ENOENT;
	}

	ret = ath12k_wmi_set_peer_param(arvif->ar, arsta->addr, arvif->vdev_id,
					WMI_PEER_PARAM_OFDMA_ENABLE,
					params->value);
	if (ret) {
		ath12k_err(arvif->ar->ab, "Failed to send WMI_PEER_PARAM_OFDMA_ENABLE to firmware\n");
		return ret;
	}

	arsta->ofdma_enable_set = true;
	arsta->ofdma_enable = params->value;

	return 0;
}

/* The value last set for the station, -1 if none since it connected */
static int ath12k_vendor_get_peer_ofdma(struct wiphy *wiphy,
					struct wireless_dev *wdev,
					struct ath12k_wifi_generic_params *params,
					u32 *value)
{
	struct ath12k_link_vif *arvif;
	struct ath12k_link_sta *arsta;

	lockdep_assert_wiphy(wiphy);

	arvif = ath12k_vendor_find_arvif(wiphy, wdev, params->link_id);
	if (!arvif)
		return -EINVAL;

	arsta = ath12k_vendor_find_arsta(arvif, params);
	if (!arsta)
		return -ENOENT;

	*value = arsta->ofdma_enable_set ? arsta->ofdma_enable : (u32)-1;

	return 0;
}

/* Extract attributes from the NL message */
static void
ath12k_vendor_wifi_extract_generic_command_params(struct nlattr **tb,
						  struct ath12k_wifi_generic_params *params)
{
	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND])
		params->command = nla_get_u32(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND]);

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_VALUE])
		params->value = nla_get_u32(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_VALUE]);

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_DATA]) {
		params->data = nla_data(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_DATA]);
		params->data_len = nla_len(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_DATA]);
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_LENGTH])
		params->length = nla_get_u32(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_LENGTH]);

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_FLAGS])
		params->flags = nla_get_u32(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_FLAGS]);

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_IFINDEX])
		params->ifindex = nla_get_u32(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_IFINDEX]);
	else
		params->ifindex = 0xFFFFFFFF;

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_MLO_LINK_ID])
		params->link_id = nla_get_u8(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_MLO_LINK_ID]);
	else
		params->link_id = INVALID_LINK_ID;

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_RADIO_INDEX])
		params->radio_idx = nla_get_u8(tb[QCA_WLAN_VENDOR_ATTR_CONFIG_RADIO_INDEX]);
	else
		params->radio_idx = INVALID_RADIO_INDEX;

	ath12k_dbg(NULL, ATH12K_DBG_CFG,
		   "wifi param: %d data: %p data len: %d ifindex: %d link id: %d radio idx: %d\n",
		   params->value, params->data, params->data_len,
		   params->ifindex, params->link_id, params->radio_idx);
}

static int ath12k_vendor_wifi_config_handler(struct wiphy *wiphy,
					     struct wireless_dev *wdev,
					     const void *data, int data_len)
{
	struct nlattr *tb[QCA_WLAN_VENDOR_ATTR_CONFIG_MAX + 1];
	struct ath12k_wifi_generic_params wifi_params;
	int ret = 0;

	ret = nla_parse(tb, QCA_WLAN_VENDOR_ATTR_CONFIG_MAX, data, data_len,
			ath12k_wifi_config_policy, NULL);

	if (ret) {
		wiphy_err(wiphy,
			  "Invalid attribute with vendor wifi config %d\n", ret);
		return ret;
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND]) {
		ath12k_dbg(NULL, ATH12K_DBG_CFG,
			   "wiphy:%p wdev: %p Extract wifi params\n",
			   wiphy, wdev);
		memset(&wifi_params, 0, sizeof(struct ath12k_wifi_generic_params));
		ath12k_vendor_wifi_extract_generic_command_params(tb, &wifi_params);
		switch (wifi_params.command) {
		case QCA_NL80211_VENDOR_SUBCMD_WIFI_PARAMS:
			if (!wifi_params.data) {
				wiphy_err(wiphy,
					  "Invalid param command received\n");
				return -EINVAL;
			}
			ret = ath12k_vendor_set_wifi_params(wiphy, wdev,
							    &wifi_params);
			if (ret) {
				wiphy_err(wiphy,
					  "Failed to set wifi params\n");
				return -EINVAL;
			}
			break;
		case QCA_WLAN_VENDOR_WIFI_PARAM_SCHED_OFDMA_ENABLE:
			ret = ath12k_vendor_set_peer_ofdma(wiphy, wdev,
							   &wifi_params);
			if (ret)
				return ret;
			break;
		default:
			ath12k_dbg(NULL, ATH12K_DBG_CFG,
				   "Un-supported generic command\n");
			return -EOPNOTSUPP;
		}
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_IF_OFFLOAD_TYPE])
		return -EOPNOTSUPP;

	return 0;
}

static int ath12k_vendor_wiphy_config_handler(struct wiphy *wiphy,
					      struct wireless_dev *wdev,
					      const void *data, int data_len)
{
	struct nlattr *tb[QCA_WLAN_VENDOR_ATTR_CONFIG_MAX + 1];
	struct ath12k_wifi_generic_params wifi_params;
	int ret = 0;

	ret = nla_parse(tb, QCA_WLAN_VENDOR_ATTR_CONFIG_MAX, data, data_len,
			ath12k_wifi_config_policy, NULL);

	if (ret) {
		wiphy_err(wiphy,
			  "Invalid attribute with vendor wiphy config %d\n", ret);
		return ret;
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND]) {
		ath12k_dbg(NULL, ATH12K_DBG_CFG,
			   "wiphy:%p wdev: %p Extract wiphy params\n",
			   wiphy, wdev);
		memset(&wifi_params, 0, sizeof(struct ath12k_wifi_generic_params));
		ath12k_vendor_wifi_extract_generic_command_params(tb, &wifi_params);
		switch (wifi_params.command) {
		case QCA_NL80211_VENDOR_SUBCMD_WIFI_PARAMS:
			if (!wifi_params.data) {
				wiphy_err(wiphy,
					  "Invalid param command received\n");
				return -EINVAL;
			}
			ret = ath12k_vendor_set_wiphy_params(wiphy,
							     &wifi_params);
			if (ret) {
				wiphy_err(wiphy,
					  "Failed to set wiphy params\n");
				return -EINVAL;
			}
			break;
		default:
			ath12k_dbg(NULL, ATH12K_DBG_CFG,
				   "Un-supported generic command\n");
			return -EOPNOTSUPP;
		}
	}

	return 0;
}

static int ath12k_vendor_get_wifi_config_handler(struct wiphy *wiphy,
						 struct wireless_dev *wdev,
						 const void *data,
						 int data_len)
{
	struct nlattr *tb[QCA_WLAN_VENDOR_ATTR_CONFIG_MAX + 1];
	struct ath12k_wifi_generic_params wifi_params;
	struct sk_buff *skb;
	int ret;
	u32 value = 0;

	ret = nla_parse(tb, QCA_WLAN_VENDOR_ATTR_CONFIG_MAX, data, data_len,
			ath12k_wifi_config_policy, NULL);
	if (ret) {
		wiphy_err(wiphy, "Invalid attribute with vendor wifi config %d\n", ret);
		return ret;
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_IF_OFFLOAD_TYPE])
		return -EOPNOTSUPP;

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND]) {
		ath12k_dbg(NULL, ATH12K_DBG_CFG,
			   "wiphy:%p wdev: %p Extract wifi params\n",
			   wiphy, wdev);
		memset(&wifi_params, 0, sizeof(struct ath12k_wifi_generic_params));
		ath12k_vendor_wifi_extract_generic_command_params(tb, &wifi_params);
		switch (wifi_params.command) {
		case QCA_NL80211_VENDOR_SUBCMD_WIFI_PARAMS:
			ret = ath12k_vendor_get_wifi_params(wiphy, wdev,
							    &wifi_params, &value);
			/* never sent to the firmware: no value */
			if (ret == -ENODATA)
				return ret;
			if (ret) {
				wiphy_err(wiphy,
					  "Failed to get wifi params\n");
				return -EINVAL;
			}
			break;
		case QCA_WLAN_VENDOR_WIFI_PARAM_SCHED_OFDMA_ENABLE:
			ret = ath12k_vendor_get_peer_ofdma(wiphy, wdev,
							   &wifi_params, &value);
			if (ret)
				return ret;
			break;
		default:
			ath12k_dbg(NULL, ATH12K_DBG_CFG,
				   "Un-supported generic command\n");
			return -EOPNOTSUPP;
		}
	}

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, NLMSG_DEFAULT_SIZE);
	if (!skb)
		return -ENOMEM;

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND]) {
		if ((nla_put_u32(skb, QCA_WLAN_VENDOR_ATTR_PARAM_DATA, value)) ||
		    (nla_put_u32(skb, QCA_WLAN_VENDOR_ATTR_PARAM_LENGTH, sizeof(u32))) ||
		    (nla_put_u32(skb, QCA_WLAN_VENDOR_ATTR_PARAM_FLAGS, 0))) {
			ret = -EINVAL;
			goto err;
		}
	}

	ret = cfg80211_vendor_cmd_reply(skb);
	if (ret) {
		wiphy_err(wiphy,
			  "send failed with err=%d\n", ret);
		return ret;
	}

	return 0;

err:
	wiphy_err(wiphy,
		  "get failed with err=%d\n", ret);
	kfree_skb(skb);
	return ret;
}

static int ath12k_vendor_get_wiphy_config_handler(struct wiphy *wiphy,
						  struct wireless_dev *wdev,
						  const void *data,
						  int data_len)
{
	struct nlattr *tb[QCA_WLAN_VENDOR_ATTR_CONFIG_MAX + 1];
	struct ath12k_wifi_generic_params wifi_params;
	struct sk_buff *skb;
	int ret;
	u32 value = 0;

	ret = nla_parse(tb, QCA_WLAN_VENDOR_ATTR_CONFIG_MAX, data, data_len,
			ath12k_wifi_config_policy, NULL);
	if (ret) {
		wiphy_err(wiphy, "Invalid attribute with vendor wiphy config %d\n", ret);
		return ret;
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND]) {
		ath12k_dbg(NULL, ATH12K_DBG_CFG,
			   "wiphy:%p wdev: %p Extract wiphy params\n",
			   wiphy, wdev);
		memset(&wifi_params, 0, sizeof(struct ath12k_wifi_generic_params));
		ath12k_vendor_wifi_extract_generic_command_params(tb, &wifi_params);
		switch (wifi_params.command) {
		case QCA_NL80211_VENDOR_SUBCMD_WIFI_PARAMS:
			ret = ath12k_vendor_get_wiphy_params(wiphy,
							     &wifi_params, &value);
			/* never sent to the firmware: no value */
			if (ret == -ENODATA)
				return ret;
			if (ret) {
				wiphy_err(wiphy,
					  "Failed to get wiphy params\n");
				return -EINVAL;
			}
			break;
		default:
			ath12k_dbg(NULL, ATH12K_DBG_CFG,
				   "Un-supported generic command\n");
			return -EOPNOTSUPP;
		}
	}

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, NLMSG_DEFAULT_SIZE);
	if (!skb)
		return -ENOMEM;

	if (tb[QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND]) {
		if ((nla_put_u32(skb, QCA_WLAN_VENDOR_ATTR_PARAM_DATA, value)) ||
		    (nla_put_u32(skb, QCA_WLAN_VENDOR_ATTR_PARAM_LENGTH, sizeof(u32))) ||
		    (nla_put_u32(skb, QCA_WLAN_VENDOR_ATTR_PARAM_FLAGS, 0))) {
			ret = -EINVAL;
			goto err;
		}
	}

	ret = cfg80211_vendor_cmd_reply(skb);
	if (ret) {
		wiphy_err(wiphy,
			  "send failed with err=%d\n", ret);
		return ret;
	}

	return 0;

err:
	wiphy_err(wiphy,
		  "put failed with err=%d\n", ret);
	kfree_skb(skb);
	return ret;
}

static const struct wiphy_vendor_command ath12k_vendor_commands[] = {
	{
		.info.vendor_id = QCA_NL80211_VENDOR_ID,
		.info.subcmd = QCA_NL80211_VENDOR_SUBCMD_SET_WIFI_CONFIGURATION,
		.doit = ath12k_vendor_wifi_config_handler,
		.policy = ath12k_wifi_config_policy,
		.maxattr = QCA_WLAN_VENDOR_ATTR_CONFIG_MAX,
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
	},
	{
		.info.vendor_id = QCA_NL80211_VENDOR_ID,
		.info.subcmd = QCA_NL80211_VENDOR_SUBCMD_GET_WIFI_CONFIGURATION,
		.doit = ath12k_vendor_get_wifi_config_handler,
		.policy = ath12k_wifi_config_policy,
		.maxattr = QCA_WLAN_VENDOR_ATTR_CONFIG_MAX,
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
	},
	{
		.info.vendor_id = QCA_NL80211_VENDOR_ID,
		.info.subcmd = QCA_NL80211_VENDOR_SUBCMD_SET_WIPHY_CONFIGURATION,
		.doit = ath12k_vendor_wiphy_config_handler,
		.policy = ath12k_wifi_config_policy,
		.maxattr = QCA_WLAN_VENDOR_ATTR_CONFIG_MAX,
	},
	{
		.info.vendor_id = QCA_NL80211_VENDOR_ID,
		.info.subcmd = QCA_NL80211_VENDOR_SUBCMD_GET_WIPHY_CONFIGURATION,
		.doit = ath12k_vendor_get_wiphy_config_handler,
		.policy = ath12k_wifi_config_policy,
		.maxattr = QCA_WLAN_VENDOR_ATTR_CONFIG_MAX,
	},
};

static const struct nl80211_vendor_cmd_info ath12k_vendor_events[] = {
	[QCA_NL80211_VENDOR_SUBCMD_IFACE_RELOAD_INDEX] = {
		.vendor_id = QCA_NL80211_VENDOR_ID,
		.subcmd = QCA_NL80211_VENDOR_SUBCMD_IFACE_RELOAD
	},
};

int ath12k_vendor_register(struct ath12k_hw *ah)
{
	ah->hw->wiphy->vendor_commands = ath12k_vendor_commands;
	ah->hw->wiphy->n_vendor_commands = ARRAY_SIZE(ath12k_vendor_commands);
	ah->hw->wiphy->vendor_events = ath12k_vendor_events;
	ah->hw->wiphy->n_vendor_events = ARRAY_SIZE(ath12k_vendor_events);
	return 0;
}
