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

/* Set link-vif level parameters
 * set 'reload' to true to send reload event to userspace
 */
static int ath12k_vendor_set_arvif_params(struct ath12k_link_vif *arvif,
					  u32 param, u32 value, bool *reload)
{
	struct ath12k *ar = arvif->ar;
	int ret = -1;

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
	case QCA_WLAN_VENDOR_VDEV_PARAM_CHWIDTH:
#define WMI_HOST_CHAN_WIDTH_MAX 0xF
		if (value > WMI_HOST_CHAN_WIDTH_MAX) {
			ath12k_err(ar->ab, "Invalid channel width value %u\n", value);
			return -EINVAL;
		}
		ret = ath12k_wmi_vdev_set_param_cmd(ar, arvif->vdev_id,
						    WMI_VDEV_PARAM_CHWIDTH, value);
		if (!ret)
			arvif->chwidth = value;
		else
			ath12k_err(ar->ab, "Failed to send WMI_VDEV_PARAM_CHWIDTH to firmware");
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

	switch (param) {
	case QCA_WLAN_VENDOR_RADIO_PARAM_TEST_RELOAD:
		*reload = true;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_CTS_TIMEOUT:
#define MAX_CTS_TIMEOUT 0xFF
		if (value > 0 && value <= MAX_CTS_TIMEOUT) {
			ret = ath12k_wmi_pdev_set_param(ar, WMI_PDEV_PARAM_CTS_TIMEOUT,
							value, ar->pdev->pdev_id);
			if (!ret)
				ar->cts_timeout = value;
			else
				ath12k_err(ar->ab, "Failed to send WMI_PDEV_PARAM_CTS_TIMEOUT to firmware");
		} else {
			ath12k_err(ar->ab, "CTS Timeout value should be between 1 and 0xFF");
			ret = -1;
		}
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_ACK_TIMEOUT:
#define MIN_TX_ACK_TIMEOUT 0x40
#define MAX_TX_ACK_TIMEOUT 0xFF
		{
			u32 rx_ack_timeout = value & 0xFFFF;
			u32 pre_rx_ack_timeout = value >> 16;

			if (rx_ack_timeout < MIN_TX_ACK_TIMEOUT ||
			    rx_ack_timeout > MAX_TX_ACK_TIMEOUT) {
				ath12k_err(ar->ab, "TX ACK Time-out value should be between 0x40 and 0xFF");
				ret = -1;
				break;
			}
			if (pre_rx_ack_timeout < MIN_TX_ACK_TIMEOUT ||
			    pre_rx_ack_timeout > MAX_TX_ACK_TIMEOUT)
				pre_rx_ack_timeout = rx_ack_timeout - 20;
			value = rx_ack_timeout | (pre_rx_ack_timeout << 16);
			ret = ath12k_wmi_pdev_set_param(ar, WMI_PDEV_PARAM_ACK_TIMEOUT,
							value, ar->pdev->pdev_id);
			if (!ret)
				ar->ack_timeout = value;
			else
				ath12k_err(ar->ab, "Failed to send WMI_PDEV_PARAM_ACK_TIMEOUT to firmware");
		}
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_SLOT_TIME:
#define MAX_SLOT_TIME 90
		if (value > 0 && value <= MAX_SLOT_TIME) {
			ret = ath12k_wmi_pdev_set_param(ar, WMI_PDEV_PARAM_SLOT_TIME,
							value, ar->pdev->pdev_id);
			if (!ret)
				ar->slot_time = value;
			else
				ath12k_err(ar->ab, "Failed to send WMI_PDEV_PARAM_SLOT_TIME to firmware");
		} else {
			ath12k_err(ar->ab, "Slot Time value should be between 1 and 90");
			ret = -1;
		}
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_EQUAL_RU_ALLOCATION_ENABLE:
		if (value > 1) {
			ath12k_err(ar->ab, "RU allocation enable value should be 0 or 1");
			ret = -1;
			break;
		}
		ret = ath12k_wmi_pdev_set_param(ar, WMI_PDEV_PARAM_EQUAL_RU_ALLOCATION_ENABLE,
						value, ar->pdev->pdev_id);
		if (!ret)
			ar->equal_ru_alloc = value;
		else
			ath12k_err(ar->ab, "Failed to send WMI_PDEV_PARAM_EQUAL_RU_ALLOCATION_ENABLE to firmware");
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
	case QCA_WLAN_VENDOR_VDEV_PARAM_CHWIDTH:
		*value = arvif->chwidth;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_FIXED_EHT_MCS:
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

	switch (param) {
	case QCA_WLAN_VENDOR_RADIO_PARAM_TEST:
		*value = 0;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_CTS_TIMEOUT:
		*value = ar->cts_timeout;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_SLOT_TIME:
		*value = ar->slot_time;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_ACK_TIMEOUT:
		*value = ar->ack_timeout;
		ret = 0;
		break;
	case QCA_WLAN_VENDOR_RADIO_PARAM_EQUAL_RU_ALLOCATION_ENABLE:
		*value = ar->equal_ru_alloc;
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
