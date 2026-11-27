/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xrdp-pon.h - contract between the hh-pon GPON MAC driver and an (open) XRDP
 * Runner driver. Proposal, docs/14 section 10; Runner side specified in docs/11.
 *
 * Shared index spaces (docs/14 section 0, item 3):
 *   flow    0..255  = GPON MAC PORT_ID slot = Runner wan_flow = BBH_TX[8] FLOW2PORT
 *                     (flow 0 is the OMCI GEM, must be < 32)
 *   channel 0..39   = WAN channel; T-CONT n uses channel n-1, the management
 *                     T-CONT (Alloc-ID = ONU-ID) uses channel 32.
 *
 * Ordering (bcmgpon): Runner flow/queue first, then MAC Port-ID enable;
 * on removal MAC Port-ID disable first, then Runner flow clear.
 */
#ifndef XRDP_PON_H
#define XRDP_PON_H

#include <linux/types.h>

struct xrdp;

enum xrdp_pon_mode {
	XRDP_PON_GPON = 1,
	XRDP_PON_XGPON = 2
};

#define XRDP_PON_OMCI_FLOW 0
#define XRDP_PON_MGMT_CHANNEL 32

struct xrdp_pon_ops { /* registered by the MAC driver */
	/* Downstream PLOAM: CPU reason 0 -> RX ring 6; 12 bytes for GPON. */
	void (*rx_ploam)(void *priv, const u8 *ploam, unsigned int len);
	void (*tx_flush_done)(void *priv, unsigned int channel);
};

/* docs/14 section 9 steps 1-7 (BBH RX/TX[8], VIQ 8/9, US TM core 3, ring 6). */
int xrdp_pon_attach(struct xrdp *x, enum xrdp_pon_mode mode, const struct xrdp_pon_ops *ops,
                    void *priv);
void xrdp_pon_detach(struct xrdp *x);

struct xrdp_gem_cfg {
	u16 port_id; /* GEM Port-ID, written to FLOW2PORT */
	bool omci;   /* size profile 1, no Ethernet FCS, CPU reason 1 -> ring 5 */
	bool ds, us;
	u8 channel;      /* upstream WAN channel */
	bool encrypt_us; /* XG-PON only */
};
int xrdp_gem_flow_set(struct xrdp *x, unsigned int flow, const struct xrdp_gem_cfg *c);
void xrdp_gem_flow_clear(struct xrdp *x, unsigned int flow);

struct xrdp_tcont_q {
	u32 drop_thresh_bytes;
	u8 weight;
};
int xrdp_tcont_set(struct xrdp *x, unsigned int channel, const struct xrdp_tcont_q *q,
                   unsigned int nq); /* strict priority, nq <= 8 */
int xrdp_tcont_enable(struct xrdp *x, unsigned int channel, bool en);
int xrdp_tcont_flush(struct xrdp *x, unsigned int channel);
int xrdp_tcont_occupancy(struct xrdp *x, unsigned int channel, u32 *bytes); /* optional, SR-DBA */

/* OMCI channel. TX: caller appends CRC-32 (GPON) / MIC (XG). Ring 19 upstream. */
int xrdp_omci_tx(struct xrdp *x, unsigned int flow, unsigned int channel, const void *msg,
                 unsigned int len);
void xrdp_omci_set_rx_handler(struct xrdp *x,
                              void (*cb)(void *priv, const void *msg, unsigned int len,
                                         unsigned int flow),
                              void *priv);

#endif /* XRDP_PON_H */
