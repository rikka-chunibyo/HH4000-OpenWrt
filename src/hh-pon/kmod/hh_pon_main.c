// SPDX-License-Identifier: GPL-2.0-only
/*
 * hh_pon - open GPON ONU driver for the BCM6858 (Bell Home Hub 4000 / Sagemcom F@ST 5689).
 *
 * SKELETON. Layering (docs/15):
 *   core/hh_gpon_fsm.c   G.984.3 O1..O7, hardware independent, host-tested
 *   this file            WAN-top SerDes + GPON MAC (docs/13) and the hh_gpon_ops binding
 *   xrdp-pon.h           Runner hand-off: PLOAM RX ring 6, OMCI ring 5/19, GEM flows, T-CONTs (docs/14)
 *
 * Safety: the laser stays off unless the module is loaded with tx_enable=1.
 * With tx_enable=0 the driver only receives (DS sync, PLOAM sniffing): a
 * misbehaving ONU transmitter can disturb every subscriber on the same PON tree.
 *
 * Tags in comments: [V] verified in docs/13-14, [I] inferred, [U] unknown/TODO.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/workqueue.h>
#include <linux/kfifo.h>
#include <linux/random.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/unaligned.h>
#include <linux/hex.h>

#include "../core/hh_gpon_fsm.h"
#include "../core/hh_gpon_calc.h"
#include "gen/hh_pon_replay.h"
#ifdef HH_PON_WITH_XRDP
#include "../include/xrdp-pon.h"
#endif

static bool tx_enable;
module_param(tx_enable, bool, 0444);
MODULE_PARM_DESC(tx_enable, "Allow the laser to transmit (default 0: receive-only)");
static char *serial;
module_param(serial, charp, 0400);
MODULE_PARM_DESC(serial, "ONU serial: 4 vendor chars + 8 hex digits (opaque config input)");
static char *password;
module_param(password, charp, 0400);
MODULE_PARM_DESC(password, "PLOAM password, up to 10 chars (opaque config input)");

/* ---- register map (physical addresses, regdb: work/regdb/{bcm_pondrv,gponstack}.h) ---- */
#define HH_WIN_BASE 0x80144000u /* WAN top .. GPON RX end */
#define HH_WIN_SIZE 0x00014000u

#define WT_MISC_3 0x8014404cu /* LASER_OE[7] LASER_MODE IFSEL */
#define WT_PMI_LP_0 0x8014405cu
#define WT_PMI_LP_1 0x80144060u
#define WT_PMI_LP_2 0x80144064u
#define WT_PMI_LP_3 0x80144068u  /* ERR[17] ACK[16] RDDATA[15:0] */
#define WT_FORCE_LBE 0x8014409cu /* [1:0]: 0 MAC, 1 force off (lbe_pol 1), 3 force on */

#define TX_TDLY 0x80148000u
#define TX_TEN 0x80148004u
#define TEN_RNGEN BIT(8)
#define TX_NPREP 0x80148010u
#define TX_TPVLD 0x80148014u
#define TX_TPCLR 0x80148020u
#define TX_TXISR0 0x80148024u
#define TXISR0_NPLMIRQ BIT(2)
#define TXISR0_RPLMIRQ BIT(5)
#define TX_TXIER0 0x80148030u
#define TX_ONUID 0x80148050u
#define TX_PROD(i) (0x80148060u + 4 * (i)) /* 16 entries */
#define TX_PRODS 0x801480a0u               /* PRCL[4:0] PRL[11:5] */
#define TX_PRRB 0x801480b0u
#define TX_DELC 0x801480c0u
#define TX_RPLM(i) (0x80148200u + 4 * (i)) /* 13 bytes */
#define TX_IPLM(i) (0x80148240u + 4 * (i))

#define RX_RCVR_STATUS 0x80150000u /* LOF[0] */
#define RX_RCVR_CONFIG 0x80150004u
#define RX_GRXISR 0x80150010u
#define GRXISR_LOFCHNG BIT(4)
#define RX_GRXIER 0x80150014u
#define RX_PLOAM_ONU_ID(i) (0x80150020u + 4 * (i)) /* [0] unicast, [1] broadcast; DISABLE[8] */
#define RX_RANG_ALC_ID 0x80150030u
#define RX_ALC_ID_CFG(i) (0x80150034u + 4 * (i)) /* i = 0..39 */
#define RX_RANG_ALC_ID_EN 0x801500d4u
#define RX_ALC_ID_EN(i) (0x801500d8u + 4 * (i))    /* 8 slots per word */
#define RX_PORT_FULLCFG(f) (0x80150200u + 4 * (f)) /* flows 0..31 */
#define FULLCFG_ENCRYPT BIT(12)
#define FULLCFG_TYPE BIT(13) /* [U] semantics */
#define RX_PORT_DISABLE(w) (0x80150600u + 4 * (w))
#define RX_ENC_KEY(i) (0x80150704u + 4 * (i))
#define RX_ENC_SWITCH_TIME 0x80150714u /* SW_TIME[29:0] ARM[30] */

#define HH_MIN_RESP_US 35     /* stock link param 0x23 [V] */
#define HH_TXIER0_STOCK 0x2c  /* NPLM, UPLM, RPLM [V] */
#define HH_GRXIER_STOCK 0x1b8 /* ACCFIFO LOFCHNG LCDGCHNG TOD FWI [V] */

/* NPLM0..3 offsets in the PLOAM memory block: NPLM3 +0xc0 .. NPLM0 +0x180 [V regdb] */
static inline u32 nplm_base(unsigned int n)
{
	return 0x80148200u + 0x180u - 0x40u * n;
}

enum hh_ev_kind {
	EV_SIGNAL,
	EV_PLOAM
};
struct hh_ev {
	u8 kind;
	u8 sig;
	struct hh_ploam_raw ploam;
};

struct hh_pon {
	struct device *dev;
	void __iomem *base;
	int irq_tx, irq_rx;
	struct gpio_desc *tx_en;   /* GPIO57, active-low in DT [V] */
	struct gpio_desc *wan_sel; /* GPIO60, 1 = GPON optics path [V userspace] */

	struct workqueue_struct *wq;
	struct work_struct ev_work;
	spinlock_t ev_lock;
	DECLARE_KFIFO(ev, struct hh_ev, 64);
	struct delayed_work to1, to2;

	struct mutex lock; /* serialises the FSM and register shadows */
	struct hh_gpon_fsm fsm;
	struct hh_gpon_ops ops;
	u32 min_resp_bits;
	struct hh_ploam_ds ovh; /* last accepted Upstream_Overhead */
	u8 random_delay;
	u16 alloc_shadow[40]; /* 0 = free; slot 0 = default Alloc (ONU-ID) */
	struct dentry *dbg;
#ifdef HH_PON_WITH_XRDP
	struct xrdp *xrdp;
#endif
};

static inline void __iomem *R(struct hh_pon *p, u32 phys)
{
	return p->base + (phys - HH_WIN_BASE);
}
static inline u32 rd(struct hh_pon *p, u32 a)
{
	return readl(R(p, a));
}
static inline void wr(struct hh_pon *p, u32 a, u32 v)
{
	writel(v, R(p, a));
}
static inline void rmw(struct hh_pon *p, u32 a, u32 clr, u32 set)
{
	wr(p, a, (rd(p, a) & ~clr) | set);
}

/* ---- WAN-top PMI bridge to the Eagle SerDes (docs/13 2.2) ---- */
static int pmi_write(struct hh_pon *p, u16 devad, u16 reg, u16 val, u16 mask)
{
	u32 st;
	int ret;

	wr(p, WT_PMI_LP_1, ((u32)devad << 16) | reg);
	wr(p, WT_PMI_LP_2, ((u32)val << 16) | (u16)~mask);
	wr(p, WT_PMI_LP_0, 1);
	wr(p, WT_PMI_LP_0, 3);
	/* Stock waits 5 us without checking; we poll ACK as docs/13 recommends. */
	ret = readl_poll_timeout_atomic(R(p, WT_PMI_LP_3), st, st & BIT(16), 1, 100);
	wr(p, WT_PMI_LP_0, 1);
	wr(p, WT_PMI_LP_0, 0);
	if (ret || (st & BIT(17))) {
		dev_warn(p->dev, "PMI write %03x:%04x failed (st %08x)\n", devad, reg, st);
		return ret ?: -EIO;
	}
	return 0;
}

static int pmi_read(struct hh_pon *p, u16 devad, u16 reg, u16 *val)
{
	u32 st;
	int ret;

	wr(p, WT_PMI_LP_1, ((u32)devad << 16) | reg);
	wr(p, WT_PMI_LP_0, 0);
	wr(p, WT_PMI_LP_0, 2);
	ret = readl_poll_timeout_atomic(R(p, WT_PMI_LP_3), st, st & BIT(16), 1, 100);
	*val = st & 0xffff;
	wr(p, WT_PMI_LP_0, 0);
	return ret;
}

static int run_replay(struct hh_pon *p, const struct hh_rp *t, unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		switch (t[i].kind) {
		case HH_RP_MMIO:
			wr(p, t[i].addr, t[i].val);
			break;
		case HH_RP_PMD:
			ret = pmi_write(p, t[i].devad, t[i].addr, t[i].val, t[i].mask);
			if (ret)
				return ret;
			break;
		case HH_RP_WAIT:
			fsleep(t[i].val);
			break;
		}
	}
	return 0;
}

static int serdes_init_gpon(struct hh_pon *p)
{
	u16 v;
	int ret = run_replay(p, hh_serdes_gpon, HH_SERDES_GPON_LEN);

	if (ret)
		return ret;
	if (!pmi_read(p, 0x800, 0xd128, &v) && !(v & BIT(8)))
		dev_warn(p->dev, "WAN PLL0 not locked (d128=%04x)\n", v); /* stock only logs */
	return 0;
}

/* ---- TX PLOAM buffers ---- */
static void write_plm(struct hh_pon *p, u32 base, const u8 b[13])
{
	unsigned int i;

	for (i = 0; i < 13; i++)
		wr(p, base + 4 * i, b[i]);
}

static void load_rplm(struct hh_pon *p, u8 onu_id)
{
	u8 b[13];

	hh_build_rplm(b, onu_id, p->fsm.cfg.sn, p->random_delay,
	              HH_OVH_OPT_POWER(p->ovh.u.overhead.options));
	write_plm(p, TX_RPLM(0), b);
}

static void load_iplm(struct hh_pon *p, u8 onu_id)
{
	u8 b[13] = {onu_id, HH_US_NO_MESSAGE};

	b[12] = hh_ploam_crc8(b, 12);
	write_plm(p, TX_IPLM(0), b);
}

static void set_tdly_preassigned(struct hh_pon *p, bool sn_state)
{
	const typeof(p->ovh.u.overhead) *o = &p->ovh.u.overhead;

	wr(p, TX_TDLY,
	   hh_tdly_encode(hh_preassigned_bits(p->min_resp_bits, o->options & HH_OVH_OPT_PRE_EQ,
	                                      o->preassigned_delay, sn_state, p->random_delay)));
}

/* ---- hh_gpon_ops implementation ---- */
static void op_set_state(void *c, enum hh_gpon_state st)
{
	struct hh_pon *p = c;

	dev_info(p->dev, "ONU state %s\n", hh_gpon_state_name(st));
}

static void op_set_onu_id(void *c, u8 id, bool valid)
{
	struct hh_pon *p = c;

	wr(p, TX_ONUID, id);
	wr(p, RX_PLOAM_ONU_ID(0), valid ? id : 0x100); /* unicast filter */
	load_rplm(p, id);
	load_iplm(p, id);
	if (valid) {
		p->random_delay = 0; /* ranging response: no random delay */
		set_tdly_preassigned(p, false);
		wr(p, RX_RANG_ALC_ID, id); /* ranging grants use Alloc-ID = ONU-ID */
		wr(p, RX_RANG_ALC_ID_EN, 1);
	} else {
		rmw(p, TX_TEN, 0, TEN_RNGEN);
		wr(p, RX_RANG_ALC_ID_EN, 0);
	}
}

static bool op_set_overhead(void *c, const struct hh_ploam_ds *d)
{
	struct hh_pon *p = c;
	const typeof(d->u.overhead) *o = &d->u.overhead;

	if (!hh_overhead_valid(o->guard_bits, o->t1_preamble_bits, o->t2_preamble_bits)) {
		dev_warn(p->dev, "invalid Upstream_Overhead GT %u T1 %u T2 %u\n", o->guard_bits,
		         o->t1_preamble_bits, o->t2_preamble_bits);
		return false; /* [V] stock rejects it and stays in O2 */
	}
	p->ovh = *d;
	/* DELC: delimiter bytes little-endian in the upper 3 bytes (ab 59 83 -> 0x8359ab00) [V] */
	wr(p, TX_DELC,
	   ((u32)o->delimiter[2] << 24) | ((u32)o->delimiter[1] << 16) |
	           ((u32)o->delimiter[0] << 8));
	wr(p, TX_PRRB, o->t3_pattern);
	/* TODO [U]: PROD[] byte image and PRODS.PRCL/PRL exactly as
	 * p_sm_standby_o2_state_upstream_overhead_message_event builds them
	 * (docs/13 5.3; compare emu/gponstack_final.txt for GT 32/T1 40/T2 64 -> PRCL 12). */
	p->random_delay = hh_random_delay(get_random_u32());
	set_tdly_preassigned(p, true);
	load_rplm(p, HH_ONU_ID_BROADCAST);
	rmw(p, TX_TEN, 0, TEN_RNGEN);
	wr(p, RX_RANG_ALC_ID, HH_ALLOC_ID_SN_REQUEST);
	wr(p, RX_RANG_ALC_ID_EN, 1);
	wr(p, RX_ALC_ID_CFG(0), 0);
	rmw(p, RX_ALC_ID_EN(0), 1, 0);
	return true;
}

static void op_set_ext_burst(void *c, u8 pre, u8 ranged)
{
	/* TODO [V regs, U exact]: DELC low byte 0xaa pattern, PRRB, PROD, PRODS.PRL = N-1;
	 * apply `ranged` again on entering O5 (docs/13 5.3). */
}

static void op_sn_response_sent(void *c)
{
	struct hh_pon *p = c;

	p->random_delay = hh_random_delay(get_random_u32());
	set_tdly_preassigned(p, true);
	load_rplm(p, HH_ONU_ID_BROADCAST);
}

static void op_set_eqd(void *c, u32 eqd_bits)
{
	struct hh_pon *p = c;

	wr(p, TX_TDLY, hh_tdly_encode(p->min_resp_bits + eqd_bits));
	if (p->fsm.state == HH_O4_RANGING) { /* first ranging: leave ranging mode [V] */
		rmw(p, TX_TEN, TEN_RNGEN, 0);
		wr(p, RX_RANG_ALC_ID_EN, 0);
		wr(p, RX_ALC_ID_CFG(0), p->fsm.onu_id); /* default Alloc = OMCC T-CONT */
		rmw(p, RX_ALC_ID_EN(0), 0, 1);
		p->alloc_shadow[0] = p->fsm.onu_id;
	}
}

static void op_laser_enable(void *c, bool on)
{
	struct hh_pon *p = c;

	if (on && !tx_enable) {
		dev_info_once(p->dev, "tx_enable=0: laser stays off (receive-only)\n");
		on = false;
	}
	wr(p, WT_FORCE_LBE, on ? 0 : 1); /* lbe_pol 1 on M02099 [V] */
	if (p->tx_en)
		gpiod_set_value_cansleep(p->tx_en, on);
}

static void op_send_ploam(void *c, const struct hh_ploam_raw *m, unsigned int repeat)
{
	struct hh_pon *p = c;
	u32 vld = rd(p, TX_TPVLD) & 0xf;
	unsigned int n;
	u8 b[13];

	/* [I] NPLM0..3 + TPVLD.NPVLD + NPREP.PREPn (docs/13 5.5; buffer writes not traced, 10). */
	for (n = 0; n < 4 && (vld & BIT(n)); n++)
		;
	if (n == 4) {
		dev_warn_ratelimited(p->dev, "US PLOAM queue full, dropping msg %u\n", m->msg_id);
		return; /* TODO: software queue refilled on NPLMIRQ */
	}
	b[0] = m->onu_id;
	b[1] = m->msg_id;
	memcpy(&b[2], m->data, HH_PLOAM_DATA_LEN);
	b[12] = hh_ploam_crc8(b, 12);
	write_plm(p, nplm_base(n), b);
	rmw(p, TX_NPREP, 3u << (2 * n), (u32)(min(repeat, 4u) - 1) << (2 * n));
	rmw(p, TX_TPVLD, 0, BIT(n));
}

static void op_timer_start(void *c, enum hh_gpon_timer t, unsigned int ms)
{
	struct hh_pon *p = c;

	mod_delayed_work(p->wq, t == HH_TIMER_TO1 ? &p->to1 : &p->to2, msecs_to_jiffies(ms));
}

static void op_timer_stop(void *c, enum hh_gpon_timer t)
{
	struct hh_pon *p = c;

	cancel_delayed_work(t == HH_TIMER_TO1 ? &p->to1 : &p->to2);
}

static void op_alloc_id(void *c, u16 alloc, bool assign)
{
	struct hh_pon *p = c;
	unsigned int i, slot = 0;

	for (i = 1; i < 40; i++) {
		if (assign && p->alloc_shadow[i] == alloc)
			return; /* duplicate assign: no write [V] */
		if (!assign && p->alloc_shadow[i] == alloc) {
			slot = i;
			break;
		}
		if (assign && !slot && !p->alloc_shadow[i])
			slot = i;
	}
	if (!slot)
		return;
	if (assign) {
		p->alloc_shadow[slot] = alloc;
		wr(p, RX_ALC_ID_CFG(slot), alloc);
		rmw(p, RX_ALC_ID_EN(slot / 8), 0, BIT(slot % 8));
	} else {
		p->alloc_shadow[slot] = 0;
		wr(p, RX_ALC_ID_CFG(slot), 0xfff);
		rmw(p, RX_ALC_ID_EN(slot / 8), BIT(slot % 8), 0);
	}
	/* T-CONT association (ALC_TCNT_ASSOC) is set later, when OMCI binds the T-CONT ME. */
}

static void op_omci_port(void *c, u16 port, bool en)
{
	struct hh_pon *p = c;

#ifdef HH_PON_WITH_XRDP
	struct xrdp_gem_cfg g = {.port_id = port,
	                         .omci = true,
	                         .ds = true,
	                         .us = true,
	                         .channel = XRDP_PON_MGMT_CHANNEL};

	if (en && xrdp_gem_flow_set(p->xrdp, XRDP_PON_OMCI_FLOW, &g)) /* Runner first [V] */
		return;
#endif
	if (en) {
		wr(p, RX_PORT_FULLCFG(0), port & 0xfff); /* TYPE/PRIORITY [U] */
		rmw(p, RX_PORT_DISABLE(0), BIT(0), 0);
	} else {
		rmw(p, RX_PORT_DISABLE(0), 0, BIT(0)); /* MAC first on removal [V] */
#ifdef HH_PON_WITH_XRDP
		xrdp_gem_flow_clear(p->xrdp, XRDP_PON_OMCI_FLOW);
#endif
	}
}

static void op_port_encrypt(void *c, u16 port, bool enc)
{
	struct hh_pon *p = c;
	unsigned int f;

	for (f = 0; f < 32; f++)
		if ((rd(p, RX_PORT_FULLCFG(f)) & 0xfff) == port)
			rmw(p, RX_PORT_FULLCFG(f), FULLCFG_ENCRYPT, enc ? FULLCFG_ENCRYPT : 0);
}

static void op_key_generate(void *c, u8 key[16])
{
	get_random_bytes(key, 16);
}

static void op_key_switch(void *c, u32 sf)
{
	struct hh_pon *p = c;
	unsigned int i;

	/* [U] byte order of KEY[0..3] relative to the PLOAM key fragments. */
	for (i = 0; i < 4; i++)
		wr(p, RX_ENC_KEY(i), get_unaligned_be32(&p->fsm.key[4 * i]));
	wr(p, RX_ENC_SWITCH_TIME, (sf & 0x3fffffff) | BIT(30));
}

static void op_link(void *c, bool up)
{
	struct hh_pon *p = c;

	dev_info(p->dev, "PON link %s\n", up ? "up" : "down");
	/* TODO: carrier of the pon0 netdev (owned by the Runner driver). */
}

/* ---- event plumbing: IRQ / Runner callback -> kfifo -> ordered workqueue ---- */
static void post(struct hh_pon *p, const struct hh_ev *e)
{
	unsigned long flags;

	spin_lock_irqsave(&p->ev_lock, flags);
	if (!kfifo_put(&p->ev, *e))
		dev_warn_ratelimited(p->dev, "event fifo full\n");
	spin_unlock_irqrestore(&p->ev_lock, flags);
	queue_work(p->wq, &p->ev_work);
}

static void post_sig(struct hh_pon *p, enum hh_gpon_signal s)
{
	struct hh_ev e = {.kind = EV_SIGNAL, .sig = s};

	post(p, &e);
}

static void ev_work(struct work_struct *w)
{
	struct hh_pon *p = container_of(w, struct hh_pon, ev_work);
	struct hh_ev e;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&p->ev_lock, flags);
		if (!kfifo_get(&p->ev, &e)) {
			spin_unlock_irqrestore(&p->ev_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&p->ev_lock, flags);
		mutex_lock(&p->lock);
		if (e.kind == EV_PLOAM)
			hh_gpon_fsm_ploam(&p->fsm, &e.ploam);
		else
			hh_gpon_fsm_signal(&p->fsm, e.sig);
		mutex_unlock(&p->lock);
	}
}

static void to1_fn(struct work_struct *w)
{
	post_sig(container_of(to_delayed_work(w), struct hh_pon, to1), HH_SIG_TO1_EXPIRED);
}

static void to2_fn(struct work_struct *w)
{
	post_sig(container_of(to_delayed_work(w), struct hh_pon, to2), HH_SIG_TO2_EXPIRED);
}

static irqreturn_t irq_rx(int irq, void *d)
{
	struct hh_pon *p = d;
	u32 isr = rd(p, RX_GRXISR);

	wr(p, RX_GRXISR, isr); /* W1C [V] */
	if (isr & GRXISR_LOFCHNG)
		post_sig(p, (rd(p, RX_RCVR_STATUS) & 1) ? HH_SIG_LOS_LOF : HH_SIG_DS_SYNC);
	return isr ? IRQ_HANDLED : IRQ_NONE;
}

static irqreturn_t irq_tx(int irq, void *d)
{
	struct hh_pon *p = d;
	u32 isr = rd(p, TX_TXISR0);

	wr(p, TX_TXISR0, isr);
	if (isr & TXISR0_RPLMIRQ)
		post_sig(p, HH_SIG_RPLM_SENT);
	/* NPLMIRQ: a normal PLOAM buffer went out; refill from a software queue (TODO). */
	return isr ? IRQ_HANDLED : IRQ_NONE;
}

#ifdef HH_PON_WITH_XRDP
static void rx_ploam(void *priv, const u8 *b, unsigned int len)
{
	struct hh_ev e = {.kind = EV_PLOAM};

	if (len < HH_PLOAM_LEN)
		return;
	e.ploam.onu_id = b[0];
	e.ploam.msg_id = b[1];
	memcpy(e.ploam.data, &b[2], HH_PLOAM_DATA_LEN);
	post(priv, &e);
}

static const struct xrdp_pon_ops hh_xrdp_ops = {.rx_ploam = rx_ploam};
#endif

/* ---- debugfs ---- */
static int state_show(struct seq_file *s, void *unused)
{
	struct hh_pon *p = s->private;

	mutex_lock(&p->lock);
	seq_printf(s, "state %s\nonu_id %d\ntx_enable %d\nploam rx %u ignored %u bad %u\n",
	           hh_gpon_state_name(p->fsm.state), p->fsm.have_onu_id ? p->fsm.onu_id : -1,
	           tx_enable, p->fsm.ploam_rx, p->fsm.ploam_ignored, p->fsm.ploam_bad);
	seq_printf(s, "rcvr_status %08x tdly %08x ten %08x\n", rd(p, RX_RCVR_STATUS),
	           rd(p, TX_TDLY), rd(p, TX_TEN));
	mutex_unlock(&p->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(state);

/* ---- probe ---- */
static int parse_identity(struct hh_pon *p, struct hh_gpon_fsm_cfg *cfg)
{
	if (!serial || strlen(serial) != 12 || hex2bin(&cfg->sn[4], serial + 4, 4)) {
		dev_err(p->dev, "serial= must be 4 vendor chars + 8 hex digits\n");
		return -EINVAL;
	}
	memcpy(cfg->sn, serial, 4);
	if (password)
		memcpy(cfg->password, password, strnlen(password, sizeof(cfg->password)));
	return 0;
}

static int hh_pon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hh_gpon_fsm_cfg cfg = {0};
	struct hh_pon *p;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	ret = parse_identity(p, &cfg);
	if (ret)
		return ret;

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);
	p->irq_tx = platform_get_irq_byname(pdev, "tx");
	p->irq_rx = platform_get_irq_byname(pdev, "rx");
	if (p->irq_tx < 0 || p->irq_rx < 0)
		return -ENXIO;
	/* Requested LOW (laser off) before anything else touches the optics. */
	p->tx_en = devm_gpiod_get_optional(dev, "tx-enable", GPIOD_OUT_LOW);
	if (IS_ERR(p->tx_en))
		return PTR_ERR(p->tx_en);
	p->wan_sel = devm_gpiod_get_optional(dev, "gpon-select", GPIOD_OUT_HIGH);
	if (IS_ERR(p->wan_sel))
		return PTR_ERR(p->wan_sel);

	mutex_init(&p->lock);
	spin_lock_init(&p->ev_lock);
	INIT_KFIFO(p->ev);
	INIT_WORK(&p->ev_work, ev_work);
	INIT_DELAYED_WORK(&p->to1, to1_fn);
	INIT_DELAYED_WORK(&p->to2, to2_fn);
	p->wq = alloc_ordered_workqueue("hh-pon", 0);
	if (!p->wq)
		return -ENOMEM;
	p->min_resp_bits = hh_min_resp_bits(HH_MIN_RESP_US);

	wr(p, WT_FORCE_LBE, 1);    /* laser forced off */
	ret = serdes_init_gpon(p); /* docs/13 2.3 */
	if (ret)
		goto err_wq;
	ret = run_replay(p, hh_mac_init, HH_MAC_INIT_LEN); /* link reset + activate, docs/13 4.3 */
	if (ret)
		goto err_wq;
	wr(p, WT_FORCE_LBE, 1); /* replay may not leave it forced */

	p->ops = (struct hh_gpon_ops){
	        .set_state = op_set_state,
	        .set_onu_id = op_set_onu_id,
	        .set_overhead = op_set_overhead,
	        .set_ext_burst = op_set_ext_burst,
	        .sn_response_sent = op_sn_response_sent,
	        .set_eqd = op_set_eqd,
	        .laser_enable = op_laser_enable,
	        .send_ploam = op_send_ploam,
	        .timer_start = op_timer_start,
	        .timer_stop = op_timer_stop,
	        .alloc_id = op_alloc_id,
	        .omci_port = op_omci_port,
	        .port_encrypt = op_port_encrypt,
	        .key_generate = op_key_generate,
	        .key_switch = op_key_switch,
	        .link = op_link,
	};
	hh_gpon_fsm_init(&p->fsm, &cfg, &p->ops, p);
	memzero_explicit(&cfg, sizeof(cfg));

	ret = devm_request_irq(dev, p->irq_rx, irq_rx, 0, "hh-pon-rx", p);
	if (!ret)
		ret = devm_request_irq(dev, p->irq_tx, irq_tx, 0, "hh-pon-tx", p);
	if (ret)
		goto err_wq;
	/* reset -> activate -> unmask (docs/13 4.1) */
	wr(p, RX_GRXISR, 0x1ff);
	wr(p, RX_GRXIER, HH_GRXIER_STOCK);
	wr(p, TX_TXIER0, HH_TXIER0_STOCK);

#ifdef HH_PON_WITH_XRDP
	/* TODO: obtain struct xrdp from the "brcm,runner" phandle (Runner driver API). */
	ret = xrdp_pon_attach(p->xrdp, XRDP_PON_GPON, &hh_xrdp_ops, p);
	if (ret)
		goto err_wq;
#else
	dev_warn(dev, "built without Runner support: no PLOAM RX, DS sync only\n");
#endif
	platform_set_drvdata(pdev, p);
	p->dbg = debugfs_create_dir("hh-pon", NULL);
	debugfs_create_file("state", 0444, p->dbg, p, &state_fops);

	post_sig(p, HH_SIG_ACTIVATE);
	if (!(rd(p, RX_RCVR_STATUS) & 1))
		post_sig(p, HH_SIG_DS_SYNC);
	dev_info(dev, "BCM6858 GPON MAC up, %s\n", tx_enable ? "TX allowed" : "receive-only");
	return 0;

err_wq:
	destroy_workqueue(p->wq);
	return ret;
}

static void hh_pon_remove(struct platform_device *pdev)
{
	struct hh_pon *p = platform_get_drvdata(pdev);

	debugfs_remove_recursive(p->dbg);
	mutex_lock(&p->lock);
	hh_gpon_fsm_signal(&p->fsm, HH_SIG_DEACTIVATE); /* laser off */
	mutex_unlock(&p->lock);
	wr(p, RX_GRXIER, 0);
	wr(p, TX_TXIER0, 0);
#ifdef HH_PON_WITH_XRDP
	xrdp_pon_detach(p->xrdp);
#endif
	cancel_delayed_work_sync(&p->to1);
	cancel_delayed_work_sync(&p->to2);
	destroy_workqueue(p->wq);
}

static const struct of_device_id hh_pon_of_match[] = {{.compatible = "brcm,bcm6858-gpon"}, {}};
MODULE_DEVICE_TABLE(of, hh_pon_of_match);

static struct platform_driver hh_pon_driver = {
        .probe = hh_pon_probe,
        .remove = hh_pon_remove,
        .driver = {.name = "hh-pon", .of_match_table = hh_pon_of_match},
};
module_platform_driver(hh_pon_driver);

MODULE_DESCRIPTION("Open GPON ONU driver for BCM6858 (HH4000) - skeleton");
MODULE_LICENSE("GPL");
