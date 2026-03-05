/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * AF_XDP zero-copy support for ath12k
 *
 * XDP/XSK callbacks are plumbed through mac80211's ieee80211_ops
 * (ndo_bpf / ndo_xsk_wakeup) so they operate on the normal wlan
 * interface (e.g. wlP2p1s0) instead of a separate bypass netdev.
 */

#ifndef ATH12K_DP_XDP_H
#define ATH12K_DP_XDP_H

#include <linux/netdevice.h>

struct ath12k_dp;
struct ath12k_base;
struct dp_rxdma_ring;
struct napi_struct;
struct ieee80211_hw;
struct ieee80211_vif;

#ifdef CONFIG_ATH12K_XDP

#include <net/xdp_sock_drv.h>
#include <net/xdp.h>
#include <linux/bpf.h>

/**
 * struct ath12k_xdp - XDP/AF_XDP zero-copy state for ath12k
 * @dp: Back pointer to ath12k_dp
 * @ab: Back pointer to ath12k_base
 * @netdev: The mac80211 wlan netdev we are associated with
 * @prog: Currently installed XDP program (RCU-protected)
 * @pool: Active XSK buffer pool (when AF_XDP ZC is enabled)
 * @rxq: XDP RX queue info for ZC (MEM_TYPE_XSK_BUFF_POOL)
 * @rxq_registered: Whether xdp_rxq_info (ZC) has been registered
 * @rxq_drv: XDP RX queue info for non-ZC native XDP (MEM_TYPE_PAGE_ORDER0)
 * @rxq_drv_registered: Whether rxq_drv has been registered
 * @napi_grp_id: ext_irq_grp index that handles the RX ring we use
 * @tx_ring_id: TCL ring index used for XSK TX
 * @tx_bank_id: TX bank profile ID for XSK TX (Ethernet encap)
 * @tx_vdev_id: VDEV ID for XSK TX descriptors
 * @tx_lmac_id: LMAC ID for XSK TX descriptors
 */
struct ath12k_xdp {
	struct ath12k_dp *dp;
	struct ath12k_base *ab;
	struct net_device *netdev;
	struct bpf_prog __rcu *prog;
	struct xsk_buff_pool *pool;
	struct xdp_rxq_info rxq;
	bool rxq_registered;
	struct xdp_rxq_info rxq_drv;
	bool rxq_drv_registered;
	int napi_grp_id;
	u8 tx_ring_id;
	int tx_bank_id;
	u32 tx_vdev_id;
	u8 tx_lmac_id;
};

/* Lifecycle – called from core.c around DP init/deinit */
int ath12k_xdp_alloc(struct ath12k_base *ab);
void ath12k_xdp_free(struct ath12k_base *ab);

/* Called from mac.c when a vif is added / removed to bind/unbind netdev */
void ath12k_xdp_set_netdev(struct ath12k_base *ab,
			    struct net_device *netdev);

/* mac80211 ieee80211_ops callbacks – registered in wifi7/hw.c */
int ath12k_xdp_mac_op_bpf(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif,
			   struct netdev_bpf *bpf);
int ath12k_xdp_mac_op_xsk_wakeup(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  u32 queue_id, u32 flags);

/* Data-path hooks – called from wifi7/dp.c */
int ath12k_xdp_rx_bufs_replenish_zc(struct ath12k_dp *dp,
				     struct dp_rxdma_ring *rx_ring,
				     struct list_head *used_list,
				     int req_entries);

int ath12k_xdp_rx_process_zc(struct ath12k_dp *dp, int ring_id,
			      struct napi_struct *napi, int budget);

void ath12k_xdp_tx_complete_zc(struct ath12k_dp *dp, int count);

/* Non-ZC XDP: run the attached XDP program on a received skb.
 * Returns XDP action (XDP_PASS, XDP_REDIRECT, XDP_DROP).
 * For XDP_REDIRECT the redirect has already been performed;
 * caller should free the skb for REDIRECT and DROP, continue for PASS.
 */
int ath12k_xdp_run_prog(struct ath12k_dp *dp, struct sk_buff *msdu,
			int *xdp_redirect_cnt);

static inline bool ath12k_xdp_is_active(struct ath12k_dp *dp)
{
	return READ_ONCE(dp->xdp) && READ_ONCE(dp->xdp->pool);
}

static inline struct xsk_buff_pool *ath12k_xdp_get_pool(struct ath12k_dp *dp)
{
	struct ath12k_xdp *xdp = READ_ONCE(dp->xdp);

	return xdp ? READ_ONCE(xdp->pool) : NULL;
}

static inline bool ath12k_xdp_has_prog(struct ath12k_dp *dp)
{
	struct ath12k_xdp *xdp_ctx = READ_ONCE(dp->xdp);

	return xdp_ctx && rcu_access_pointer(xdp_ctx->prog);
}

#else /* !CONFIG_ATH12K_XDP */

static inline int ath12k_xdp_alloc(struct ath12k_base *ab) { return 0; }
static inline void ath12k_xdp_free(struct ath12k_base *ab) {}
static inline void ath12k_xdp_set_netdev(struct ath12k_base *ab,
					  struct net_device *netdev) {}
static inline bool ath12k_xdp_is_active(struct ath12k_dp *dp) { return false; }
static inline bool ath12k_xdp_has_prog(struct ath12k_dp *dp) { return false; }
static inline int ath12k_xdp_run_prog(struct ath12k_dp *dp, struct sk_buff *msdu,
				      int *xdp_redirect_cnt) { return XDP_PASS; }

#endif /* CONFIG_ATH12K_XDP */

#endif /* ATH12K_DP_XDP_H */
