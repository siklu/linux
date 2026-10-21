/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* QCA nl80211 vendor commands of the QSDK cfg80211tool.
 *
 * Ported from the QSDK NHSS.QSDK.13.1.5-01089 ath12k driver
 * (oss/src/mac80211/wlan-open 3314b0461628, vendor.h). Only the generic
 * Wi-Fi and wiphy configuration commands are carried; the numbers are kept
 * unchanged so the QSDK 13.1 cfg80211tool and command tables work as is.
 */

#ifndef ATH12K_VENDOR_H
#define ATH12K_VENDOR_H

#define QCA_NL80211_VENDOR_ID 0x001374

#define INVALID_LINK_ID 0xFF
#define INVALID_RADIO_INDEX 0xFF

struct ath12k_hw;

struct ath12k_wifi_generic_params {
	u32 command;
	u32 value;
	void *data;
	u32 data_len;
	u32 length;
	u32 flags;
	u32 ifindex;
	u8 link_id;
	u8 radio_idx;
};

enum qca_nl80211_vendor_subcmds {
	/* Wi-Fi configuration subcommand */
	QCA_NL80211_VENDOR_SUBCMD_SET_WIFI_CONFIGURATION = 74,
	QCA_NL80211_VENDOR_SUBCMD_GET_WIFI_CONFIGURATION = 75,
	QCA_NL80211_VENDOR_SUBCMD_WIFI_PARAMS = 200,
	QCA_NL80211_VENDOR_SUBCMD_IFACE_RELOAD = 267,
	QCA_NL80211_VENDOR_SUBCMD_SET_WIPHY_CONFIGURATION = 268,
	QCA_NL80211_VENDOR_SUBCMD_GET_WIPHY_CONFIGURATION = 269,
};

/* Indexes into ath12k_vendor_events[] */
enum qca_nl80211_vendor_events {
	QCA_NL80211_VENDOR_SUBCMD_IFACE_RELOAD_INDEX = 0,
};

enum qca_wlan_vendor_attr_config {
	QCA_WLAN_VENDOR_ATTR_CONFIG_INVALID = 0,
	/* Unsigned 32-bit attribute for generic commands */
	QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_COMMAND = 17,
	/* Unsigned 32-bit value attribute for generic commands */
	QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_VALUE = 18,
	/* Unsigned 32-bit data attribute for generic command response */
	QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_DATA = 19,
	/* Unsigned 32-bit length attribute for
	 * QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_DATA
	 */
	QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_LENGTH = 20,
	/* Unsigned 32-bit flags attribute for
	 * QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_DATA
	 */
	QCA_WLAN_VENDOR_ATTR_CONFIG_GENERIC_FLAGS = 21,
	/* Unsigned 32-bit, specifies the interface index (netdev) for which the
	 * corresponding configurations are applied
	 */
	QCA_WLAN_VENDOR_ATTR_CONFIG_IFINDEX = 24,
	/* 8-bit unsigned value, link id of a multi-link (MLD) netdev */
	QCA_WLAN_VENDOR_ATTR_CONFIG_MLO_LINK_ID = 99,
	/* 8-bit unsigned value, PPE offload type of the interface. Not
	 * supported: this driver has no PPE offload.
	 */
	QCA_WLAN_VENDOR_ATTR_IF_OFFLOAD_TYPE = 120,
	/* 8-bit unsigned value, radio index of a single-wiphy netdev */
	QCA_WLAN_VENDOR_ATTR_CONFIG_RADIO_INDEX = 135,

	/* keep last */
	QCA_WLAN_VENDOR_ATTR_CONFIG_AFTER_LAST,
	QCA_WLAN_VENDOR_ATTR_CONFIG_MAX =
		QCA_WLAN_VENDOR_ATTR_CONFIG_AFTER_LAST - 1
};

/* Reply attributes of the GET_*_CONFIGURATION subcommands */
enum qca_wlan_genric_data {
	QCA_WLAN_VENDOR_ATTR_GENERIC_PARAM_INVALID = 0,
	QCA_WLAN_VENDOR_ATTR_PARAM_DATA,
	QCA_WLAN_VENDOR_ATTR_PARAM_LENGTH,
	QCA_WLAN_VENDOR_ATTR_PARAM_FLAGS,

	/* keep last */
	QCA_WLAN_VENDOR_ATTR_GENERIC_PARAM_LAST,
	QCA_WLAN_VENDOR_ATTR_GENERIC_PARAM_MAX =
	QCA_WLAN_VENDOR_ATTR_GENERIC_PARAM_LAST - 1
};

enum qca_wlan_vendor_attr_iface_reload {
	QCA_WLAN_VENDOR_ATTR_IFACE_RELOAD_INVALID = 0,
	QCA_WLAN_VENDOR_ATTR_IFACE_RELOAD_LINKID = 1,

	/* Keep last */
	QCA_WLAN_VENDOR_ATTR_IFACE_RELOAD_AFTER_LAST,
	QCA_WLAN_VENDOR_ATTR_IFACE_RELOAD_MAX =
	QCA_WLAN_VENDOR_ATTR_IFACE_RELOAD_AFTER_LAST - 1,
};

enum qca_vendor_vdev_param {
	QCA_WLAN_VENDOR_VDEV_PARAM_TEST = 0,
	QCA_WLAN_VENDOR_VDEV_PARAM_TEST_RELOAD = QCA_WLAN_VENDOR_VDEV_PARAM_TEST,

	/* Add new params above */
	QCA_WLAN_VENDOR_VDEV_PARAM_LAST,
	QCA_WLAN_VENDOR_VDEV_PARAM_MAX = QCA_WLAN_VENDOR_VDEV_PARAM_LAST - 1,
};

enum qca_vendor_radio_param {
	QCA_WLAN_VENDOR_RADIO_PARAM_TEST = 0,
	QCA_WLAN_VENDOR_RADIO_PARAM_TEST_RELOAD = QCA_WLAN_VENDOR_RADIO_PARAM_TEST,
	/* From QSDK 14.0 (NHSS.QSDK.14.0-01668) */
	QCA_WLAN_VENDOR_RADIO_PARAM_CTS_TIMEOUT = 57,
	QCA_WLAN_VENDOR_RADIO_PARAM_SLOT_TIME = 58,
	/* RX ACK timeout in bits 0-15, pre-RX ACK timeout in bits 16-31 */
	QCA_WLAN_VENDOR_RADIO_PARAM_ACK_TIMEOUT = 59,

	/* Add new params above */
	QCA_WLAN_VENDOR_RADIO_PARAM_LAST,
	QCA_WLAN_VENDOR_RADIO_PARAM_MAX = QCA_WLAN_VENDOR_RADIO_PARAM_LAST - 1,
};

#define ATH_PARAM_MASK     0x1000
enum ath_cfg_param_radio {
	ACFG_PARAM_RADIO_TXCHAINMASK	      = 1  | ATH_PARAM_MASK,
	ACFG_PARAM_RADIO_RXCHAINMASK	      = 2  | ATH_PARAM_MASK,
	PARAM_RADIO_TXCHAINSOFT               = 361 | ATH_PARAM_MASK,
};

int ath12k_vendor_register(struct ath12k_hw *ah);

#endif /* ATH12K_VENDOR_H */
