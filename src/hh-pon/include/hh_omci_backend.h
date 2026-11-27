/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hh_omci_backend.h - southbound interface of an open OMCI daemon on the HH4000.
 *
 * The OMCI engine (MIB, message handling: docs/12) is backend-agnostic. It turns
 * OLT provisioning into the calls below. Two backends are planned:
 *
 *  1. "stock" - hybrid track, stock 4.1 kernel + Broadcom modules:
 *       transport  /dev/bcm_omci (318): one frame per read(), driver adds CRC on write
 *       GEM/T-CONT /dev/bcm_ploam (319) ioctls 0x71/0x73/0x74 (GEM), 0x88/0x89 (Alloc-ID)
 *       queues     libtmctl -> /dev/rdpa_user
 *       VLAN       bcmvlan rules on veip0 (stock applies them 1 s after the last change)
 *     (docs/12 "Kernel interface" and "Service model", docs/14 section 7)
 *
 *  2. "hh-pon" - native track, modern kernel + hh-pon.ko + open Runner driver:
 *       transport  generic netlink family "hh-pon" (or the net/xpon OMCI endpoint)
 *       GEM/T-CONT/queues  hh-pon genl commands -> xrdp-pon.h
 *       VLAN       802.1Q/bridge config on pon0 via rtnetlink (OpenWrt netifd)
 *
 * All IDs are as received from the OLT; the backend owns the mapping to flow
 * slots (0..255) and WAN channels (0..39).
 */
#ifndef HH_OMCI_BACKEND_H
#define HH_OMCI_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum hh_gem_dir {
	HH_GEM_DS = 1,
	HH_GEM_US = 2,
	HH_GEM_BIDIR = 3
};

struct hh_gem {
	uint16_t port_id;  /* GEM Port Network CTP attr 1 */
	uint16_t tcont_me; /* T-CONT ME instance (0x8000..) or 0 */
	enum hh_gem_dir dir;
	bool encrypt;
	bool multicast;
	uint16_t us_queue_me; /* priority queue ME (0x8000..) */
};

struct hh_queue {
	uint16_t me_id; /* priority queue ME */
	uint16_t tcont_me;
	uint8_t prio; /* 0 = highest (docs/12 open question: q vs 7-q) */
	uint8_t weight;
	uint32_t drop_thresh_bytes;
};

/* Simplified extended-VLAN rule (ME 171), already resolved against the UNI. */
struct hh_vlan_rule {
	uint16_t filter_outer_vid, filter_inner_vid; /* 4096 = any, 4097 = untagged */
	uint8_t filter_outer_pri, filter_inner_pri;  /* 8 = any */
	uint16_t filter_ethertype;                   /* 0 = any */
	uint8_t remove_tags;                         /* 0..2 */
	uint16_t add_outer_vid, add_inner_vid;       /* 4096 = none */
	uint8_t add_outer_pri, add_inner_pri;
	uint16_t gem_port_id; /* bound mapper/GEM, 0 = any */
};

struct hh_omci_backend {
	const char *name;
	void *priv;

	/* Transport: baseline frames are 44 bytes without CRC/MIC (48 with). */
	int (*open)(void *priv);
	int (*poll_fd)(void *priv);
	int (*recv)(void *priv, uint8_t *buf, size_t len);       /* one frame, >0 = length */
	int (*send)(void *priv, const uint8_t *buf, size_t len); /* -ENOLINK before O5 */

	/* Provisioning */
	int (*tcont_bind)(void *priv, uint16_t tcont_me,
	                  uint16_t alloc_id); /* alloc 0xFF/0xFFFF = unbind */
	int (*queue_set)(void *priv, const struct hh_queue *q);
	int (*gem_add)(void *priv, const struct hh_gem *g);
	int (*gem_del)(void *priv, uint16_t port_id);
	int (*vlan_apply)(void *priv, uint16_t uni_me, const struct hh_vlan_rule *rules,
	                  unsigned int n); /* replaces the set */

	/* Status used by ME attributes and alarms (ANI-G optical levels, ONU-G). */
	int (*optics)(void *priv, int32_t *rx_dbm_x500, int32_t *tx_dbm_x500, int32_t *temp_c_x256,
	              uint32_t *bias_ua);
	int (*link_state)(void *priv, int *ploam_state); /* 1..7 */
	int (*reboot)(void *priv);
};

#endif /* HH_OMCI_BACKEND_H */
