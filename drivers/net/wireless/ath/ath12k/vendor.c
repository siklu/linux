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

/* Set link-vif level parameters
 * set 'reload' to true to send reload event to userspace
 */
static int ath12k_vendor_set_arvif_params(struct ath12k *ar, u32 param,
					  u32 value, bool *reload)
{
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
	struct ath12k *ar;
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

	ar = arvif->ar;
	rcu_read_unlock();

	ath12k_dbg(NULL, ATH12K_DBG_CFG,
		   "vif: %p param: %d value: %d if: %d link: %d\n",
		   vif, param, value,
		   params->ifindex, params->link_id);

	ret = ath12k_vendor_set_arvif_params(ar, param, value, &reload);

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
	case QCA_WLAN_VENDOR_VDEV_PARAM_TEST:
		*value = 0;
		ret = 0;
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
