// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * AF_XDP zero-copy support for ath12k
 *
 * This module provides a dedicated bypass netdev (ath12kxdp%d) that
 * supports XDP programs and AF_XDP zero-copy sockets.  When an AF_XDP
 * socket is bound to this netdev the driver switches the shared RXDMA
 * refill ring to use XSK buffer pool allocations and runs XDP programs
 * on received frames before (optionally) redirecting them to the
 * AF_XDP socket.
 *
 * Architecturally ath12k uses a single RXDMA refill ring that feeds
 * all REO destination rings.  When XSK zero-copy is active **all** RX
 * buffers come from the XSK pool – normal mac80211 traffic on the same
 * device is delivered via XDP_PASS (converted from xdp_buff to skb).
 */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <net/xdp_sock_drv.h>
#include <net/xdp.h>

#include "core.h"
#include "dp.h"
#include "dp_rx.h"
#include "dp_tx.h"
#include "dp_xdp.h"
#include "hal.h"
#include "debug.h"
#include "wifi7/hal_desc.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/**
 * ath12k_xdp_find_rx_napi_grp - find IRQ group that services RX ring 0
 * @ab: device base
 *
 * Returns the ext_irq_grp index whose ring_mask->rx[] covers REO dst
 * ring 0, or -1 if not found.
 */
static int ath12k_xdp_find_rx_napi_grp(struct ath12k_base *ab)
{
	int i;

	for (i = 0; i < ATH12K_EXT_IRQ_GRP_NUM_MAX; i++) {
		if (ab->hw_params->ring_mask->rx[i] & BIT(0))
			return i;
	}
	return -1;
}

/**
 * ath12k_xdp_construct_skb - build an skb from an XSK xdp_buff (XDP_PASS)
 *
 * When the XDP program returns XDP_PASS we need to hand the frame to
 * the normal stack / mac80211.  This copies the data into a fresh skb.
 */
static struct sk_buff *ath12k_xdp_construct_skb(struct xdp_buff *xdp,
						struct net_device *ndev)
{
	unsigned int totalsize = xdp->data_end - xdp->data;
	unsigned int metasize = xdp->data - xdp->data_meta;
	struct sk_buff *skb;

	skb = napi_alloc_skb(NULL, totalsize);
	if (!skb)
		return NULL;

	memcpy(__skb_put(skb, totalsize), xdp->data_meta, totalsize);
	if (metasize) {
		skb_metadata_set(skb, metasize);
		__skb_pull(skb, metasize);
	}

	skb->dev = ndev;
	return skb;
}

/* ------------------------------------------------------------------ */
/*  RX zero-copy buffer replenish                                      */
/* ------------------------------------------------------------------ */

/**
 * ath12k_xdp_rx_bufs_replenish_zc - replenish RXDMA ring with XSK buffers
 * @dp:          ath12k_dp context
 * @rx_ring:     RXDMA refill ring
 * @used_list:   list of ath12k_rx_desc_info returned after RX processing
 * @req_entries: how many entries to fill (0 = as many as possible)
 *
 * This is the zero-copy replacement for ath12k_dp_rx_bufs_replenish().
 * Instead of dev_alloc_skb + dma_map_single it allocates buffers from the
 * XSK buffer pool whose DMA mapping was established at pool setup time.
 */
int ath12k_xdp_rx_bufs_replenish_zc(struct ath12k_dp *dp,
				     struct dp_rxdma_ring *rx_ring,
				     struct list_head *used_list,
				     int req_entries)
{
	struct ath12k_base *ab = dp->ab;
	struct ath12k_xdp *xdp_ctx = dp->xdp;
	struct xsk_buff_pool *pool = xdp_ctx->pool;
	struct ath12k_buffer_addr *desc;
	struct hal_srng *srng;
	struct xdp_buff *xdp;
	struct ath12k_rx_desc_info *rx_desc;
	dma_addr_t dma;
	u32 cookie;
	int num_free, num_remain;
	enum hal_rx_buf_return_buf_manager mgr = dp->hal->hal_params->rx_buf_rbm;

	req_entries = min(req_entries, rx_ring->bufs_max);

	srng = &dp->hal->srng_list[rx_ring->refill_buf_ring.ring_id];

	spin_lock_bh(&srng->lock);

	ath12k_hal_srng_access_begin(ab, srng);

	num_free = ath12k_hal_srng_src_num_free(ab, srng, true);
	if (!req_entries && (num_free > (rx_ring->bufs_max * 3) / 4))
		req_entries = num_free;

	req_entries = min(num_free, req_entries);
	num_remain = req_entries;

	if (!num_remain)
		goto out;

	/* Grab descriptors from free list if used_list is empty */
	if (list_empty(used_list)) {
		struct list_head *cur;
		struct ath12k_rx_desc_info *rd;
		int count = 0;
		int to_cut = num_remain;

		spin_lock_bh(&dp->rx_desc_lock);
		list_for_each(cur, &dp->rx_desc_free_list) {
			if (!to_cut)
				break;
			rd = list_entry(cur, struct ath12k_rx_desc_info, list);
			rd->in_use = true;
			to_cut--;
			count++;
		}
		list_cut_before(used_list, &dp->rx_desc_free_list, cur);
		spin_unlock_bh(&dp->rx_desc_lock);
		num_remain = count;
	}

	while (num_remain > 0) {
		xdp = xsk_buff_alloc(pool);
		if (!xdp)
			break;

		dma = xsk_buff_xdp_get_dma(xdp);

		rx_desc = list_first_entry_or_null(used_list,
						   struct ath12k_rx_desc_info,
						   list);
		if (!rx_desc) {
			xsk_buff_free(xdp);
			break;
		}

		desc = ath12k_hal_srng_src_get_next_entry(ab, srng);
		if (!desc) {
			xsk_buff_free(xdp);
			break;
		}

		/* Store XSK buffer in desc_info */
		rx_desc->skb = NULL;
		rx_desc->xdp = xdp;
		rx_desc->xsk_buf = 1;
		cookie = rx_desc->cookie;

		list_del(&rx_desc->list);

		num_remain--;

		ath12k_hal_rx_buf_addr_info_set(dp->hal, desc, dma, cookie,
						mgr);
	}

out:
	ath12k_hal_srng_access_end(ab, srng);

	/* Return unused descriptors to free list */
	if (!list_empty(used_list)) {
		struct ath12k_rx_desc_info *d, *safe;

		list_for_each_entry_safe(d, safe, used_list, list)
			d->in_use = false;

		spin_lock_bh(&dp->rx_desc_lock);
		list_splice_tail(used_list, &dp->rx_desc_free_list);
		spin_unlock_bh(&dp->rx_desc_lock);
	}

	spin_unlock_bh(&srng->lock);

	return req_entries - num_remain;
}
EXPORT_SYMBOL(ath12k_xdp_rx_bufs_replenish_zc);

/* ------------------------------------------------------------------ */
/*  RX zero-copy processing (NAPI poll)                                */
/* ------------------------------------------------------------------ */

/**
 * ath12k_xdp_rx_process_zc - process REO destination ring entries (ZC mode)
 *
 * This replaces ath12k_wifi7_dp_rx_process() when XSK is active.  It reaps
 * completed descriptors, runs the XDP program, and either redirects to the
 * AF_XDP socket (XDP_REDIRECT) or drops / passes upward.
 */
int ath12k_xdp_rx_process_zc(struct ath12k_dp *dp, int ring_id,
			      struct napi_struct *napi, int budget)
{
	struct ath12k_xdp *xdp_ctx = dp->xdp;
	struct ath12k_base *ab = dp->ab;
	struct ath12k_hal *hal = dp->hal;
	struct xsk_buff_pool *pool = xdp_ctx->pool;
	struct bpf_prog *xdp_prog;
	struct hal_reo_dest_ring *reo_desc;
	struct ath12k_rx_desc_info *desc_info;
	struct dp_rxdma_ring *rx_ring;
	struct hal_srng *srng;
	LIST_HEAD(used_list);
	struct xdp_buff *xdp;
	struct sk_buff *skb;
	u32 hal_desc_sz = hal->hal_desc_sz;
	int total_reaped = 0;
	int xdp_xmit = 0;
	u32 act;
	u64 desc_va;
	int num_buffs_reaped = 0;

	rcu_read_lock();
	xdp_prog = rcu_dereference(xdp_ctx->prog);
	if (!xdp_prog) {
		rcu_read_unlock();
		return 0;
	}

	srng = &hal->srng_list[dp->reo_dst_ring[ring_id].ring_id];

	spin_lock_bh(&srng->lock);
	ath12k_hal_srng_access_begin(ab, srng);

	while (budget > 0 &&
	       (reo_desc = ath12k_hal_srng_dst_get_next_entry(ab, srng))) {
		struct hal_rx_desc_data rx_info = {};
		u32 push_reason;
		struct hal_rx_desc *rx_desc;
		u16 msdu_len;
		u8 l3_pad;

		desc_va = ((u64)le32_to_cpu(reo_desc->buf_va_hi) << 32 |
			   le32_to_cpu(reo_desc->buf_va_lo));
		desc_info = (struct ath12k_rx_desc_info *)((unsigned long)desc_va);

		if (unlikely(!desc_info)) {
			u32 cookie = le32_get_bits(reo_desc->buf_addr_info.info1,
						   BUFFER_ADDR_INFO1_SW_COOKIE);
			desc_info = ath12k_dp_get_rx_desc(dp, cookie);
			if (!desc_info)
				continue;
		}

		if (desc_info->magic != ATH12K_DP_RX_DESC_MAGIC)
			ath12k_warn(ab, "xdp zc: bad desc magic\n");

		push_reason = le32_get_bits(reo_desc->info0,
					    HAL_REO_DEST_RING_INFO0_PUSH_REASON);

		/* Handle non-XSK buffers that are still in flight from
		 * before the pool was enabled (should be rare/transient).
		 */
		if (unlikely(!desc_info->xsk_buf)) {
			struct sk_buff *msdu = desc_info->skb;

			desc_info->skb = NULL;
			if (msdu) {
				struct ath12k_skb_rxcb *rxcb = ATH12K_SKB_RXCB(msdu);

				dma_unmap_single(dp->dev, rxcb->paddr,
						 msdu->len + skb_tailroom(msdu),
						 DMA_FROM_DEVICE);
				dev_kfree_skb_any(msdu);
			}
			list_add_tail(&desc_info->list, &used_list);
			num_buffs_reaped++;
			continue;
		}

		xdp = desc_info->xdp;
		desc_info->xdp = NULL;
		desc_info->xsk_buf = 0;

		list_add_tail(&desc_info->list, &used_list);
		num_buffs_reaped++;

		if (push_reason != HAL_REO_DEST_RING_PUSH_REASON_ROUTING_INSTRUCTION) {
			xsk_buff_free(xdp);
			dp->device_stats.hal_reo_error[ring_id]++;
			continue;
		}

		/* DMA sync to make HW-written data visible to CPU */
		xsk_buff_dma_sync_for_cpu(xdp);

		/* Read the HW rx descriptor at the start of the buffer.
		 * After xsk_buff_alloc() xdp->data points to where HW
		 * wrote:  [hal_rx_desc | l3_pad | msdu_data ...]
		 */
		rx_desc = (struct hal_rx_desc *)xdp->data;

		/* Extract MSDU length and L3 padding from the HW descriptor */
		ath12k_dp_extract_rx_desc_data(hal, &rx_info, rx_desc, rx_desc);
		msdu_len = rx_info.msdu_len;
		l3_pad = rx_info.l3_pad_bytes;

		/* Sanity check */
		if (unlikely(hal_desc_sz + l3_pad + msdu_len > DP_RX_BUFFER_SIZE)) {
			xsk_buff_free(xdp);
			continue;
		}

		/* Adjust xdp_buff to frame the actual packet data,
		 * skipping over the HW rx descriptor + L3 padding.
		 * data_hard_start remains at the chunk start for headroom.
		 */
		xdp->data += hal_desc_sz + l3_pad;
		xdp->data_end = xdp->data + msdu_len;
		xdp->data_meta = xdp->data;

		xdp_init_buff(xdp, xsk_pool_get_rx_frame_size(pool) +
				    (xdp->data - xdp->data_hard_start),
			       &xdp_ctx->rxq);

		/* Run XDP program */
		act = bpf_prog_run_xdp(xdp_prog, xdp);

		switch (act) {
		case XDP_REDIRECT:
			if (xdp_do_redirect(xdp_ctx->netdev, xdp, xdp_prog)) {
				xsk_buff_free(xdp);
			} else {
				xdp_xmit++;
			}
			break;
		case XDP_PASS:
			skb = ath12k_xdp_construct_skb(xdp, xdp_ctx->netdev);
			xsk_buff_free(xdp);
			if (skb)
				netif_receive_skb(skb);
			break;
		case XDP_TX:
			/* For XDP_TX we'd need to loop back through the
			 * TX path.  For now treat as redirect-to-self or
			 * drop with a stat.  Full XDP_TX can be added
			 * later.
			 */
			xsk_buff_free(xdp);
			break;
		case XDP_DROP:
		default:
			xsk_buff_free(xdp);
			break;
		}

		total_reaped++;
		budget--;
	}

	ath12k_hal_srng_access_end(ab, srng);
	spin_unlock_bh(&srng->lock);

	if (xdp_xmit)
		xdp_do_flush();

	/* Replenish consumed buffers with fresh XSK allocations */
	if (num_buffs_reaped) {
		rx_ring = &dp->rx_refill_buf_ring;
		ath12k_xdp_rx_bufs_replenish_zc(dp, rx_ring, &used_list,
						 num_buffs_reaped);
	}

	/* Wakeup management */
	if (xsk_uses_need_wakeup(pool)) {
		if (total_reaped == 0)
			xsk_set_rx_need_wakeup(pool);
		else
			xsk_clear_rx_need_wakeup(pool);
	}

	rcu_read_unlock();

	return total_reaped;
}
EXPORT_SYMBOL(ath12k_xdp_rx_process_zc);

/* ------------------------------------------------------------------ */
/*  TX zero-copy completion                                            */
/* ------------------------------------------------------------------ */

/**
 * ath12k_xdp_tx_complete_zc - notify XSK pool of completed TX frames
 * @dp:    ath12k_dp context
 * @count: number of XSK TX frames that completed
 */
void ath12k_xdp_tx_complete_zc(struct ath12k_dp *dp, int count)
{
	struct ath12k_xdp *xdp_ctx = dp->xdp;

	if (xdp_ctx && xdp_ctx->pool && count)
		xsk_tx_completed(xdp_ctx->pool, count);
}
EXPORT_SYMBOL(ath12k_xdp_tx_complete_zc);

/* ------------------------------------------------------------------ */
/*  TX zero-copy transmit (called from ndo_xsk_wakeup)                 */
/* ------------------------------------------------------------------ */

/**
 * ath12k_xdp_tx_zc - transmit frames from the XSK TX queue
 * @xdp_ctx: XDP context
 *
 * Pulls descriptors from the AF_XDP TX ring and submits them into the
 * ath12k TCL data ring.  Returns number of frames submitted.
 */
static int ath12k_xdp_tx_zc(struct ath12k_xdp *xdp_ctx)
{
	struct ath12k_dp *dp = xdp_ctx->dp;
	struct ath12k_base *ab = dp->ab;
	struct ath12k_hal *hal = dp->hal;
	struct xsk_buff_pool *pool = xdp_ctx->pool;
	struct dp_tx_ring *tx_ring;
	struct hal_srng *tcl_ring;
	struct hal_tcl_data_cmd *tcl_desc;
	struct ath12k_tx_desc_info *tx_desc;
	struct xdp_desc desc;
	dma_addr_t dma;
	int budget, submitted = 0;
	u8 ring_id = xdp_ctx->tx_ring_id;
	u8 pool_id;

	tx_ring = &dp->tx_ring[ring_id];
	tcl_ring = &hal->srng_list[tx_ring->tcl_data_ring.ring_id];

	spin_lock_bh(&tcl_ring->lock);
	ath12k_hal_srng_access_begin(ab, tcl_ring);

	budget = ath12k_hal_srng_src_num_free(ab, tcl_ring, true);
	budget = min(budget, 64); /* cap per-wakeup batch */

	while (budget > 0 && xsk_tx_peek_desc(pool, &desc)) {
		if (desc.len == 0 || desc.len > DP_RX_BUFFER_SIZE) {
			xsk_tx_release(pool);
			continue;
		}

		pool_id = smp_processor_id() & (ATH12K_HW_MAX_QUEUES - 1);
		tx_desc = ath12k_dp_tx_assign_buffer(dp, pool_id);
		if (!tx_desc)
			break;

		tcl_desc = ath12k_hal_srng_src_get_next_entry(ab, tcl_ring);
		if (!tcl_desc) {
			ath12k_dp_tx_release_txbuf(dp, tx_desc, pool_id);
			break;
		}

		dma = xsk_buff_raw_get_dma(pool, desc.addr);
		xsk_buff_raw_dma_sync_for_device(pool, dma, desc.len);

		/* Mark as XSK TX so completion handler knows not to
		 * free an skb.
		 */
		tx_desc->skb = NULL;
		tx_desc->skb_ext_desc = NULL;
		tx_desc->mac_id = 0;

		/* Build TCL descriptor for Ethernet-encapsulated frame.
		 * Inlined here to avoid a cross-module call into
		 * ath12k_wifi7 which would create a circular module
		 * dependency (ath12k <-> ath12k_wifi7).
		 */
		tcl_desc->buf_addr_info.info0 =
			le32_encode_bits(dma, BUFFER_ADDR_INFO0_ADDR);
		tcl_desc->buf_addr_info.info1 =
			le32_encode_bits(((u64)dma >> HAL_ADDR_MSB_REG_SHIFT),
					 BUFFER_ADDR_INFO1_ADDR) |
			le32_encode_bits(hal->tcl_to_wbm_rbm_map[ring_id].rbm_id,
					 BUFFER_ADDR_INFO1_RET_BUF_MGR) |
			le32_encode_bits(tx_desc->desc_id,
					 BUFFER_ADDR_INFO1_SW_COOKIE);
		tcl_desc->info0 =
			le32_encode_bits(HAL_TCL_DESC_TYPE_BUFFER,
					 HAL_TCL_DATA_CMD_INFO0_DESC_TYPE) |
			le32_encode_bits(xdp_ctx->tx_bank_id,
					 HAL_TCL_DATA_CMD_INFO0_BANK_ID);
		tcl_desc->info1 = 0;
		tcl_desc->info2 =
			le32_encode_bits(desc.len,
					 HAL_TCL_DATA_CMD_INFO2_DATA_LEN);
		tcl_desc->info3 =
			le32_encode_bits(1, HAL_TCL_DATA_CMD_INFO3_TID_OVERWRITE) |
			le32_encode_bits(0, HAL_TCL_DATA_CMD_INFO3_TID) |
			le32_encode_bits(xdp_ctx->tx_lmac_id,
					 HAL_TCL_DATA_CMD_INFO3_PMAC_ID) |
			le32_encode_bits(xdp_ctx->tx_vdev_id,
					 HAL_TCL_DATA_CMD_INFO3_VDEV_ID);
		tcl_desc->info4 = 0;
		tcl_desc->info5 = 0;

		xsk_tx_release(pool);
		submitted++;
		budget--;
	}

	ath12k_hal_srng_access_end(ab, tcl_ring);
	spin_unlock_bh(&tcl_ring->lock);

	if (xsk_uses_need_wakeup(pool))
		xsk_set_tx_need_wakeup(pool);

	return submitted;
}

/* ------------------------------------------------------------------ */
/*  Netdev operations for the XDP bypass interface                     */
/* ------------------------------------------------------------------ */

static int ath12k_xdp_ndev_open(struct net_device *dev)
{
	netif_carrier_on(dev);
	netif_tx_start_all_queues(dev);
	return 0;
}

static int ath12k_xdp_ndev_stop(struct net_device *dev)
{
	netif_carrier_off(dev);
	netif_tx_stop_all_queues(dev);
	return 0;
}

static netdev_tx_t ath12k_xdp_ndev_xmit(struct sk_buff *skb,
					 struct net_device *dev)
{
	/* This netdev is primarily for AF_XDP zero-copy.
	 * Non-XDP skb TX is not supported through this interface.
	 */
	dev_kfree_skb_any(skb);
	dev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

/* ------------------------------------------------------------------ */
/*  XDP program setup                                                  */
/* ------------------------------------------------------------------ */

static int ath12k_xdp_setup_prog(struct ath12k_xdp *xdp_ctx,
				  struct bpf_prog *prog,
				  struct netlink_ext_ack *extack)
{
	struct bpf_prog *old_prog;

	old_prog = rcu_replace_pointer(xdp_ctx->prog, prog,
				       lockdep_rtnl_is_held());
	if (old_prog)
		bpf_prog_put(old_prog);

	return 0;
}

/* ------------------------------------------------------------------ */
/*  XSK buffer pool setup / teardown                                   */
/* ------------------------------------------------------------------ */

static int ath12k_xdp_pool_enable(struct ath12k_xdp *xdp_ctx,
				   struct xsk_buff_pool *pool)
{
	struct ath12k_dp *dp = xdp_ctx->dp;
	struct ath12k_base *ab = xdp_ctx->ab;
	struct dp_rxdma_ring *rx_ring = &dp->rx_refill_buf_ring;
	u32 frame_sz;
	int ret, grp_id;
	unsigned int napi_id = 0;
	LIST_HEAD(list);

	frame_sz = xsk_pool_get_rx_frame_size(pool);
	if (frame_sz < DP_RX_BUFFER_SIZE) {
		ath12k_warn(ab,
			    "XSK frame size %u too small, need at least %d\n",
			    frame_sz, DP_RX_BUFFER_SIZE);
		return -EINVAL;
	}

	ret = xsk_pool_dma_map(pool, dp->dev, 0);
	if (ret) {
		ath12k_warn(ab, "failed to DMA-map XSK pool: %d\n", ret);
		return ret;
	}

	/* Find the NAPI that handles REO dst ring 0 for rxq_info */
	grp_id = ath12k_xdp_find_rx_napi_grp(ab);
	if (grp_id >= 0) {
		napi_id = ab->ext_irq_grp[grp_id].napi.napi_id;
		xdp_ctx->napi_grp_id = grp_id;
	}

	/* Register xdp_rxq_info on the XDP netdev */
	if (xdp_ctx->rxq_registered)
		xdp_rxq_info_unreg(&xdp_ctx->rxq);

	ret = xdp_rxq_info_reg(&xdp_ctx->rxq, xdp_ctx->netdev, 0, napi_id);
	if (ret) {
		ath12k_warn(ab, "failed to register xdp_rxq_info: %d\n", ret);
		goto err_dma_unmap;
	}
	xdp_ctx->rxq_registered = true;

	ret = xdp_rxq_info_reg_mem_model(&xdp_ctx->rxq,
					  MEM_TYPE_XSK_BUFF_POOL, NULL);
	if (ret) {
		ath12k_warn(ab, "failed to register XSK mem model: %d\n", ret);
		goto err_rxq_unreg;
	}

	xsk_pool_set_rxq_info(pool, &xdp_ctx->rxq);

	/* Activate the pool - from this point replenish uses XSK buffers */
	WRITE_ONCE(xdp_ctx->pool, pool);

	/* Replenish the RXDMA refill ring with XSK buffers.
	 * Existing sk_buff-based entries will be consumed naturally
	 * by HW and handled by the ZC RX path's legacy fallback.
	 */
	ath12k_xdp_rx_bufs_replenish_zc(dp, rx_ring, &list, 0);

	ath12k_info(ab, "XSK buffer pool enabled on %s (frame_sz=%u)\n",
		    netdev_name(xdp_ctx->netdev), frame_sz);

	return 0;

err_rxq_unreg:
	xdp_rxq_info_unreg(&xdp_ctx->rxq);
	xdp_ctx->rxq_registered = false;
err_dma_unmap:
	xsk_pool_dma_unmap(pool, 0);
	return ret;
}

static void ath12k_xdp_pool_disable(struct ath12k_xdp *xdp_ctx)
{
	struct xsk_buff_pool *pool = xdp_ctx->pool;

	if (!pool)
		return;

	ath12k_info(xdp_ctx->ab, "XSK buffer pool disabled on %s\n",
		    netdev_name(xdp_ctx->netdev));

	WRITE_ONCE(xdp_ctx->pool, NULL);

	/* Ensure no NAPI is accessing the pool */
	synchronize_net();

	if (xdp_ctx->rxq_registered) {
		xdp_rxq_info_unreg(&xdp_ctx->rxq);
		xdp_ctx->rxq_registered = false;
	}

	xsk_pool_dma_unmap(pool, 0);
}

static int ath12k_xdp_pool_setup(struct ath12k_xdp *xdp_ctx,
				  struct xsk_buff_pool *pool, u16 queue_id)
{
	if (queue_id > 0)
		return -EINVAL;

	if (pool)
		return ath12k_xdp_pool_enable(xdp_ctx, pool);

	ath12k_xdp_pool_disable(xdp_ctx);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  ndo_bpf callback                                                   */
/* ------------------------------------------------------------------ */

static int ath12k_xdp_ndo_bpf(struct net_device *dev, struct netdev_bpf *bpf)
{
	struct ath12k_xdp *xdp_ctx = netdev_priv(dev);

	switch (bpf->command) {
	case XDP_SETUP_PROG:
		return ath12k_xdp_setup_prog(xdp_ctx, bpf->prog, bpf->extack);
	case XDP_SETUP_XSK_POOL:
		return ath12k_xdp_pool_setup(xdp_ctx, bpf->xsk.pool,
					     bpf->xsk.queue_id);
	default:
		return -EINVAL;
	}
}

/* ------------------------------------------------------------------ */
/*  ndo_xsk_wakeup callback                                            */
/* ------------------------------------------------------------------ */

static int ath12k_xdp_wakeup(struct net_device *dev, u32 queue_id, u32 flags)
{
	struct ath12k_xdp *xdp_ctx = netdev_priv(dev);
	struct ath12k_base *ab = xdp_ctx->ab;

	if (!netif_carrier_ok(dev))
		return -ENETDOWN;

	if (queue_id > 0)
		return -EINVAL;

	if (!xdp_ctx->pool)
		return -EINVAL;

	/* Handle TX wakeup: transmit pending XSK TX frames */
	if (flags & XDP_WAKEUP_TX)
		ath12k_xdp_tx_zc(xdp_ctx);

	/* Trigger NAPI to process any pending RX */
	if (flags & XDP_WAKEUP_RX) {
		int grp_id = xdp_ctx->napi_grp_id;

		if (grp_id >= 0 && grp_id < ATH12K_EXT_IRQ_GRP_NUM_MAX) {
			struct napi_struct *napi = &ab->ext_irq_grp[grp_id].napi;

			if (!napi_if_scheduled_mark_missed(napi))
				napi_schedule(napi);
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/*  Netdev ops table                                                   */
/* ------------------------------------------------------------------ */

static const struct net_device_ops ath12k_xdp_netdev_ops = {
	.ndo_open		= ath12k_xdp_ndev_open,
	.ndo_stop		= ath12k_xdp_ndev_stop,
	.ndo_start_xmit		= ath12k_xdp_ndev_xmit,
	.ndo_bpf		= ath12k_xdp_ndo_bpf,
	.ndo_xsk_wakeup		= ath12k_xdp_wakeup,
};

/* ------------------------------------------------------------------ */
/*  XDP netdev creation / destruction                                  */
/* ------------------------------------------------------------------ */

static void ath12k_xdp_ndev_setup(struct net_device *dev)
{
	dev->netdev_ops = &ath12k_xdp_netdev_ops;
	dev->type = ARPHRD_NONE;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
	dev->priv_flags |= IFF_NO_QUEUE;
	dev->features |= NETIF_F_SG | NETIF_F_HW_CSUM;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_DATA_LEN;
	dev->mtu = ETH_DATA_LEN;
	eth_hw_addr_random(dev);
}

/**
 * ath12k_xdp_create - create the XDP bypass netdev for an ath12k device
 * @ab: ath12k_base
 *
 * Creates a netdev named "ath12kxdp%d" that VPP (or any AF_XDP consumer)
 * can bind to for zero-copy operation.
 */
int ath12k_xdp_create(struct ath12k_base *ab)
{
	struct ath12k_dp *dp = ath12k_ab_to_dp(ab);
	struct net_device *ndev;
	struct ath12k_xdp *xdp_ctx;
	int ret;

	ndev = alloc_netdev(sizeof(*xdp_ctx), "ath12kxdp%d",
			    NET_NAME_ENUM, ath12k_xdp_ndev_setup);
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, ab->dev);

	xdp_ctx = netdev_priv(ndev);
	xdp_ctx->netdev = ndev;
	xdp_ctx->dp = dp;
	xdp_ctx->ab = ab;
	xdp_ctx->pool = NULL;
	xdp_ctx->rxq_registered = false;
	xdp_ctx->napi_grp_id = ath12k_xdp_find_rx_napi_grp(ab);
	xdp_ctx->tx_ring_id = 0;
	xdp_ctx->tx_bank_id = 0;
	xdp_ctx->tx_vdev_id = 0;
	xdp_ctx->tx_lmac_id = 0;

	ret = register_netdev(ndev);
	if (ret) {
		ath12k_err(ab, "failed to register XDP netdev: %d\n", ret);
		free_netdev(ndev);
		return ret;
	}

	WRITE_ONCE(dp->xdp, xdp_ctx);
	ath12k_info(ab, "created XDP interface %s\n", netdev_name(ndev));

	return 0;
}

/**
 * ath12k_xdp_destroy - tear down the XDP bypass netdev
 * @ab: ath12k_base
 */
void ath12k_xdp_destroy(struct ath12k_base *ab)
{
	struct ath12k_dp *dp = ath12k_ab_to_dp(ab);
	struct ath12k_xdp *xdp_ctx;

	xdp_ctx = dp->xdp;
	if (!xdp_ctx)
		return;

	WRITE_ONCE(dp->xdp, NULL);
	synchronize_rcu();

	/* Tear down pool if still active */
	ath12k_xdp_pool_disable(xdp_ctx);

	/* Remove XDP program */
	ath12k_xdp_setup_prog(xdp_ctx, NULL, NULL);

	unregister_netdev(xdp_ctx->netdev);
	free_netdev(xdp_ctx->netdev);
}
