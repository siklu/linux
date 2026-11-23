// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* QCA nl80211 vendor commands of the QSDK cfg80211tool, with the parameters
 * of long-distance links and link tuning.
 *
 * Ported from the QSDK NHSS.QSDK.13.1.5-01089 ath12k driver
 * (oss/src/mac80211/wlan-open 3314b0461628, vendor.c): the generic Wi-Fi
 * (per interface) and wiphy (per radio) configuration commands.
 * Differences from the QSDK code:
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

/* A vendor parameter that is one WMI pdev parameter. As in the QSDK 14.0
 * setters, the value is checked and sent as is. The meaning of the values
 * is in the firmware's wmi_unified.h.
 *
 * The firmware cannot be asked for these parameters, so the getters return
 * the value last sent to it. A parameter never sent has the firmware's
 * default, which the driver does not know; its getter fails with -ENODATA.
 */
struct ath12k_vendor_wmi_param {
	u32 param;
	u32 wmi_param;
	u32 min;
	u32 max;
	/* further checks, may change the value */
	int (*check)(struct ath12k *ar, u32 *value);
};

#define ATH12K_VENDOR_PDEV_WMI(_param, _wmi, _min, _max, ...)		\
	{ .param = QCA_WLAN_VENDOR_RADIO_PARAM_ ## _param,		\
	  .wmi_param = WMI_PDEV_PARAM_ ## _wmi,				\
	  .min = (_min), .max = (_max), __VA_ARGS__ }

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

static const struct ath12k_vendor_wmi_param ath12k_vendor_pdev_wmi_params[] = {
	ATH12K_VENDOR_PDEV_WMI(UL_OFDMA_RTD, UL_OFDMA_RTD, 0, 600),
	/* us */
	ATH12K_VENDOR_PDEV_WMI(CTS_TIMEOUT, CTS_TIMEOUT, 1, 0xff),
	ATH12K_VENDOR_PDEV_WMI(SLOT_TIME, SLOT_TIME, 1, 90),
	ATH12K_VENDOR_PDEV_WMI(ACK_TIMEOUT, ACK_TIMEOUT, 0, U32_MAX,
			       .check = ath12k_vendor_check_ack_timeout),
};

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

/* Record a WMI pdev parameter sent to the firmware, by anyone */
void ath12k_vendor_pdev_param_sent(struct ath12k *ar, u32 wmi_param,
				   u32 value)
{
	struct ath12k_vendor_wmi_vals *vals = &ar->vendor_pdev_wmi;
	int i;

	for (i = 0; i < ARRAY_SIZE(ath12k_vendor_pdev_wmi_params); i++) {
		if (ath12k_vendor_pdev_wmi_params[i].wmi_param != wmi_param)
			continue;

		WRITE_ONCE(vals->val[i], value);
		/* the value before the bit, for ath12k_vendor_get_wmi_val() */
		smp_mb__before_atomic();
		set_bit(i, vals->sent);
	}
}

static int ath12k_vendor_get_wmi_val(struct ath12k_vendor_wmi_vals *vals,
				     int idx, u32 *value)
{
	if (!test_bit(idx, vals->sent))
		return -ENODATA;

	/* the bit before the value, see ath12k_vendor_pdev_param_sent() */
	smp_rmb();
	*value = READ_ONCE(vals->val[idx]);

	return 0;
}

static int ath12k_vendor_check_wmi_param(struct ath12k *ar,
					 const struct ath12k_vendor_wmi_param *wp,
					 u32 *value)
{
	if (*value < wp->min || *value > wp->max) {
		ath12k_err(ar->ab, "Value %u should be %u to %u\n",
			   *value, wp->min, wp->max);
		return -EINVAL;
	}

	return wp->check ? wp->check(ar, value) : 0;
}

static int ath12k_vendor_set_pdev_wmi_param(struct ath12k *ar, int idx,
					    u32 value)
{
	const struct ath12k_vendor_wmi_param *wp =
		&ath12k_vendor_pdev_wmi_params[idx];
	int ret;

	ret = ath12k_vendor_check_wmi_param(ar, wp, &value);
	if (ret)
		return ret;

	ret = ath12k_wmi_pdev_set_param(ar, wp->wmi_param, value,
					ar->pdev->pdev_id);
	if (ret)
		ath12k_err(ar->ab, "Failed to send pdev param %u to firmware\n",
			   wp->wmi_param);

	return ret;
}

/* Set link-vif level parameters */
static int ath12k_vendor_set_arvif_params(struct ath12k_link_vif *arvif,
					  u32 param, u32 value)
{
	struct ath12k *ar = arvif->ar;
	int ret = -1;

	switch (param) {
	case QCA_WLAN_VENDOR_VDEV_PARAM_FIXED_EHT_MCS:
		ret = ath12k_vendor_set_fixed_eht_mcs(arvif, value);
		break;
	case QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_OFDMA:
	case QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_OFDMA:
	case QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_MUMIMO:
	case QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_MUMIMO:
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

/* Set radio level parameters */
static int ath12k_vendor_set_radio_params(struct ath12k *ar,
					  u32 param, u32 value)
{
	int ret = -1;
	int idx;

	idx = ath12k_vendor_find_wmi_param(ath12k_vendor_pdev_wmi_params,
					   ARRAY_SIZE(ath12k_vendor_pdev_wmi_params),
					   param);
	if (idx >= 0)
		ret = ath12k_vendor_set_pdev_wmi_param(ar, idx, value);
	else
		ath12k_dbg(ar->ab, ATH12K_DBG_CFG,
			   "Un-supported param: %d\n", param);

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
	u32 value;

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

	return ath12k_vendor_set_arvif_params(arvif, param, value);
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

	return ath12k_vendor_set_radio_params(ar, param, value);
}

/* Get link-vif level parameters */
static int ath12k_vendor_get_arvif_params(struct ath12k_link_vif *arvif,
					  u32 param, u32 *value)
{
	int ret = -1;

	switch (param) {
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
	int idx;

	idx = ath12k_vendor_find_wmi_param(ath12k_vendor_pdev_wmi_params,
					   ARRAY_SIZE(ath12k_vendor_pdev_wmi_params),
					   param);
	if (idx >= 0)
		ret = ath12k_vendor_get_wmi_val(&ar->vendor_pdev_wmi, idx, value);
	else
		ath12k_dbg(ar->ab, ATH12K_DBG_CFG,
			   "Un-supported param: %d\n", param);

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

int ath12k_vendor_register(struct ath12k_hw *ah)
{
	ah->hw->wiphy->vendor_commands = ath12k_vendor_commands;
	ah->hw->wiphy->n_vendor_commands = ARRAY_SIZE(ath12k_vendor_commands);
	return 0;
}
