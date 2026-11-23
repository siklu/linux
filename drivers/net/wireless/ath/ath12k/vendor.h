/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* QCA nl80211 vendor commands of the QSDK cfg80211tool.
 *
 * Ported from the QSDK NHSS.QSDK.13.1.5-01089 ath12k driver
 * (oss/src/mac80211/wlan-open 3314b0461628, vendor.h). Only the generic
 * Wi-Fi and wiphy configuration commands are carried, with the parameters
 * of long-distance links and link tuning; the numbers are kept unchanged
 * so the QSDK cfg80211tool and command tables work as is. The parameters
 * marked QSDK 14.0 keep their QSDK 14.0 numbers. The ones QSDK does not
 * have are numbered from 1001, clear of the QSDK numbers.
 */

#ifndef ATH12K_VENDOR_H
#define ATH12K_VENDOR_H

#define QCA_NL80211_VENDOR_ID 0x001374

#define INVALID_LINK_ID 0xFF
#define INVALID_RADIO_INDEX 0xFF

struct ath12k;
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
	QCA_NL80211_VENDOR_SUBCMD_SET_WIPHY_CONFIGURATION = 268,
	QCA_NL80211_VENDOR_SUBCMD_GET_WIPHY_CONFIGURATION = 269,
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

/* Interface parameters, SET/GET_WIFI_CONFIGURATION */
enum qca_vendor_vdev_param {
	/* From QSDK 14.0 */
	QCA_WLAN_VENDOR_VDEV_PARAM_FIXED_EHT_MCS = 77,
	/* From QSDK 14.0. 0 or 1: one bit of the HE or EHT MU mode */
	QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_OFDMA = 84,
	QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_OFDMA = 87,
	/* Not in QSDK. 0 or 1: bits of the HE or EHT MU mode */
	QCA_WLAN_VENDOR_VDEV_PARAM_HE_UL_MUMIMO = 1001,
	QCA_WLAN_VENDOR_VDEV_PARAM_EHT_UL_MUMIMO = 1002,
	/* Not in QSDK. Best effort EDCA parameters of the interface:
	 * CW min and CW max (2^n - 1), AIFSN, TXOP limit in units of 32 us
	 */
	QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMIN = 1005,
	QCA_WLAN_VENDOR_VDEV_PARAM_BE_CWMAX = 1006,
	QCA_WLAN_VENDOR_VDEV_PARAM_BE_AIFS = 1007,
	QCA_WLAN_VENDOR_VDEV_PARAM_BE_TXOP = 1008,
};

/* Radio parameters, SET/GET_WIPHY_CONFIGURATION. From QSDK 14.0
 * (NHSS.QSDK.14.0-01668), one WMI pdev parameter each, see
 * ath12k_vendor_pdev_wmi_params[].
 */
enum qca_vendor_radio_param {
	QCA_WLAN_VENDOR_RADIO_PARAM_UL_OFDMA_RTD = 33,
	QCA_WLAN_VENDOR_RADIO_PARAM_CTS_TIMEOUT = 57,
	QCA_WLAN_VENDOR_RADIO_PARAM_SLOT_TIME = 58,
	/* RX ACK timeout in bits 0-15, pre-RX ACK timeout in bits 16-31 */
	QCA_WLAN_VENDOR_RADIO_PARAM_ACK_TIMEOUT = 59,
};

int ath12k_vendor_register(struct ath12k_hw *ah);
void ath12k_vendor_pdev_param_sent(struct ath12k *ar, u32 wmi_param,
				   u32 value);

#endif /* ATH12K_VENDOR_H */
