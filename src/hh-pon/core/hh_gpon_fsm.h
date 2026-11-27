/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hh_gpon_fsm.h - G.984.3 ONU activation state machine (O1..O7).
 *
 * Hardware-independent: all side effects go through struct hh_gpon_ops, so the
 * same code runs in the kernel module (ops = BCM6858 GPON MAC, docs/13) and in
 * host unit tests (ops = recorder). Transitions follow G.984.3 clause 10.2,
 * Table 10-1. Design informed by odi-oss odi_gpon_fsm.c (GPL-2.0), written
 * independently.
 *
 * Threading: the caller serialises all hh_gpon_fsm_* calls (kernel: one
 * workqueue or a spinlock; PLOAM RX from Runner ring 6 and timers both funnel
 * into it).
 */
#ifndef HH_GPON_FSM_H
#define HH_GPON_FSM_H

#include "hh_ploam.h"

enum hh_gpon_state {
	HH_O1_INITIAL = 1,
	HH_O2_STANDBY,
	HH_O3_SERIAL_NUMBER,
	HH_O4_RANGING,
	HH_O5_OPERATION,
	HH_O6_POPUP,
	HH_O7_EMERGENCY_STOP,
};

enum hh_gpon_timer {
	HH_TIMER_TO1, /* SN + ranging timer, O3..O4 (stock default 20 s) */
	HH_TIMER_TO2, /* POPUP timer, O6 (default 100 ms) */
};

enum hh_gpon_signal {
	HH_SIG_DS_SYNC, /* downstream frame sync acquired (LOS/LOF cleared) */
	HH_SIG_LOS_LOF, /* loss of signal or loss of frame */
	HH_SIG_TO1_EXPIRED,
	HH_SIG_TO2_EXPIRED,
	HH_SIG_RPLM_SENT,  /* TXISR0.RPLMIRQ: MAC sent the ranging PLOAM buffer */
	HH_SIG_ACTIVATE,   /* admin: (re)start from O1 */
	HH_SIG_DEACTIVATE, /* admin: laser off, back to O1 and stay there */
};

struct hh_gpon_ops {
	void (*set_state)(void *ctx, enum hh_gpon_state st);
	void (*set_onu_id)(void *ctx, uint8_t onu_id, bool valid);
	/* Returns false if the overhead is rejected (stock: T1+T2 > 128 or not byte aligned). */
	bool (*set_overhead)(void *ctx, const struct hh_ploam_ds *overhead);
	void (*set_eqd)(void *ctx, uint32_t eqd_bits);
	void (*set_ext_burst)(void *ctx, uint8_t t3_preranged, uint8_t t3_ranged);
	/* O3: SN_ONU was sent from the RPLM buffer; rewrite random delay (TDLY, RPLM). */
	void (*sn_response_sent)(void *ctx);
	void (*laser_enable)(void *ctx, bool on);
	/* Queue an upstream PLOAM, sent `repeat` times on successive PLOAMu grants. */
	void (*send_ploam)(void *ctx, const struct hh_ploam_raw *m, unsigned int repeat);
	void (*timer_start)(void *ctx, enum hh_gpon_timer t, unsigned int ms);
	void (*timer_stop)(void *ctx, enum hh_gpon_timer t);
	/* Alloc-ID (T-CONT) and GEM port provisioning by PLOAM. */
	void (*alloc_id)(void *ctx, uint16_t alloc_id, bool assign);
	void (*omci_port)(void *ctx, uint16_t port_id, bool enable);
	void (*port_encrypt)(void *ctx, uint16_t port_id, bool encrypt);
	/* Encryption: generate a fresh 128-bit key, latch it at a superframe. */
	void (*key_generate)(void *ctx, uint8_t key[16]);
	void (*key_switch)(void *ctx, uint32_t superframe);
	void (*ber_interval)(void *ctx, uint32_t frames);
	/* O5 entered/left: lets the datapath bring the WAN netdev carrier up/down. */
	void (*link)(void *ctx, bool up);
};

struct hh_gpon_fsm_cfg {
	uint8_t sn[8];        /* Vendor-ID + VSSN, opaque configuration input */
	uint8_t password[10]; /* opaque configuration input */
	unsigned int to1_ms;  /* 0 -> 10000 */
	unsigned int to2_ms;  /* 0 -> 100 */
};

struct hh_gpon_fsm {
	enum hh_gpon_state state;
	struct hh_gpon_fsm_cfg cfg;
	const struct hh_gpon_ops *ops;
	void *ctx;
	uint8_t onu_id;
	bool have_onu_id;
	bool admin_down;
	bool ds_synced;
	uint8_t key_index;
	uint8_t key[16];
	/* statistics */
	uint32_t ploam_rx, ploam_ignored, ploam_bad;
};

void hh_gpon_fsm_init(struct hh_gpon_fsm *f, const struct hh_gpon_fsm_cfg *cfg,
                      const struct hh_gpon_ops *ops, void *ctx);
void hh_gpon_fsm_signal(struct hh_gpon_fsm *f, enum hh_gpon_signal s);
void hh_gpon_fsm_ploam(struct hh_gpon_fsm *f, const struct hh_ploam_raw *m);
const char *hh_gpon_state_name(enum hh_gpon_state st);

#endif /* HH_GPON_FSM_H */
