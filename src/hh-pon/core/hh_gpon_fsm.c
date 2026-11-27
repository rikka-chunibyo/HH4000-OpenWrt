// SPDX-License-Identifier: GPL-2.0-only
/*
 * hh_gpon_fsm.c - G.984.3 ONU activation state machine. See hh_gpon_fsm.h.
 *
 * Interop notes (docs/09 section 4, from odi-oss / OpenWrt EcoNet experience):
 *  - Directed messages are only "ours" while we hold an ONU-ID (O4..O6).
 *  - Extended_Burst_Length is only applied in O3.
 *  - Acknowledge only directed (non-broadcast) messages that require it.
 *  - Password and key fragments are sent three times each.
 */
#include "hh_gpon_fsm.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define CALL(f, op, ...)                                                                           \
	do {                                                                                       \
		if ((f)->ops->op)                                                                  \
			(f)->ops->op((f)->ctx, ##__VA_ARGS__);                                     \
	} while (0)

static const char *const names[] = {
        "?",          "O1-Initial",   "O2-Standby", "O3-SerialNumber",
        "O4-Ranging", "O5-Operation", "O6-Popup",   "O7-EmergencyStop",
};

const char *hh_gpon_state_name(enum hh_gpon_state st)
{
	return (st >= HH_O1_INITIAL && st <= HH_O7_EMERGENCY_STOP) ? names[st] : names[0];
}

static void discard_onu_id(struct hh_gpon_fsm *f)
{
	if (!f->have_onu_id)
		return;
	f->have_onu_id = false;
	f->onu_id = HH_ONU_ID_BROADCAST;
	CALL(f, set_onu_id, HH_ONU_ID_BROADCAST, false);
}

static void enter(struct hh_gpon_fsm *f, enum hh_gpon_state st)
{
	enum hh_gpon_state old = f->state;

	if (old == st)
		return;
	if (old == HH_O5_OPERATION)
		CALL(f, link, false);
	/* TO1 covers O3 and O4 as one interval (stock gponstack, docs/13 5.7):
	 * started on US_Overhead (O2->O3) or broadcast POPUP (O6->O4), kept
	 * across O3->O4, stopped on any other exit. */
	if ((old == HH_O3_SERIAL_NUMBER && st != HH_O4_RANGING) || old == HH_O4_RANGING)
		CALL(f, timer_stop, HH_TIMER_TO1);
	if (old == HH_O6_POPUP)
		CALL(f, timer_stop, HH_TIMER_TO2);

	f->state = st;
	CALL(f, set_state, st);

	switch (st) {
	case HH_O1_INITIAL:
	case HH_O2_STANDBY:
	case HH_O7_EMERGENCY_STOP:
		discard_onu_id(f);
		CALL(f, laser_enable, false);
		break;
	case HH_O3_SERIAL_NUMBER:
		/* Laser on at first US_Overhead, as stock (docs/13 3). From here
		 * the MAC answers SN grants itself from the RPLM buffer. */
		CALL(f, laser_enable, true);
		CALL(f, timer_start, HH_TIMER_TO1, f->cfg.to1_ms);
		break;
	case HH_O4_RANGING:
		CALL(f, laser_enable, true);
		if (old == HH_O6_POPUP)
			CALL(f, timer_start, HH_TIMER_TO1, f->cfg.to1_ms);
		break;
	case HH_O5_OPERATION:
		CALL(f, laser_enable, true);
		CALL(f, link, true);
		break;
	case HH_O6_POPUP:
		/* No upstream transmission while waiting for POPUP. Stock only
		 * clears ALC_ID_EN here; switching the laser off is stricter. */
		CALL(f, laser_enable, false);
		CALL(f, timer_start, HH_TIMER_TO2, f->cfg.to2_ms);
		break;
	}
}

void hh_gpon_fsm_init(struct hh_gpon_fsm *f, const struct hh_gpon_fsm_cfg *cfg,
                      const struct hh_gpon_ops *ops, void *ctx)
{
	memset(f, 0, sizeof(*f));
	f->cfg = *cfg;
	if (!f->cfg.to1_ms)
		f->cfg.to1_ms = 20000; /* stock link parameter; G.984.3 suggests 10 s */
	if (!f->cfg.to2_ms)
		f->cfg.to2_ms = 100;
	f->ops = ops;
	f->ctx = ctx;
	f->onu_id = HH_ONU_ID_BROADCAST;
	f->state = HH_O1_INITIAL;
	f->admin_down = true; /* nothing happens until HH_SIG_ACTIVATE */
	CALL(f, laser_enable, false);
	CALL(f, set_state, HH_O1_INITIAL);
}

void hh_gpon_fsm_signal(struct hh_gpon_fsm *f, enum hh_gpon_signal s)
{
	switch (s) {
	case HH_SIG_ACTIVATE:
		f->admin_down = false;
		enter(f, HH_O1_INITIAL);
		if (f->ds_synced)
			enter(f, HH_O2_STANDBY);
		break;
	case HH_SIG_DEACTIVATE:
		f->admin_down = true;
		enter(f, HH_O1_INITIAL);
		break;
	case HH_SIG_DS_SYNC:
		f->ds_synced = true;
		if (f->state == HH_O1_INITIAL && !f->admin_down)
			enter(f, HH_O2_STANDBY);
		break;
	case HH_SIG_LOS_LOF:
		f->ds_synced = false;
		switch (f->state) {
		case HH_O2_STANDBY:
		case HH_O3_SERIAL_NUMBER:
		case HH_O4_RANGING:
			enter(f, HH_O1_INITIAL);
			break;
		case HH_O5_OPERATION:
			enter(f, HH_O6_POPUP);
			break;
		default: /* O1, O6 (TO2 running), O7 (sticky) */
			break;
		}
		break;
	case HH_SIG_TO1_EXPIRED:
		if (f->state == HH_O3_SERIAL_NUMBER || f->state == HH_O4_RANGING)
			enter(f, HH_O2_STANDBY);
		break;
	case HH_SIG_RPLM_SENT:
		/* MAC sent our SN_ONU in an SN grant: pick a new random delay. */
		if (f->state == HH_O3_SERIAL_NUMBER)
			CALL(f, sn_response_sent);
		break;
	case HH_SIG_TO2_EXPIRED:
		if (f->state == HH_O6_POPUP)
			enter(f, HH_O1_INITIAL);
		break;
	}
}

static bool sn_match(const struct hh_gpon_fsm *f, const uint8_t sn[8])
{
	return memcmp(f->cfg.sn, sn, 8) == 0;
}

static void ack(struct hh_gpon_fsm *f, const struct hh_ploam_raw *m)
{
	struct hh_ploam_raw a;

	if (m->onu_id == HH_ONU_ID_BROADCAST)
		return;
	hh_ploam_encode_ack(&a, f->onu_id, m);
	CALL(f, send_ploam, &a, 1);
}

static void send_key(struct hh_gpon_fsm *f)
{
	struct hh_ploam_raw m;

	CALL(f, key_generate, f->key);
	f->key_index++;
	hh_ploam_encode_key(&m, f->onu_id, f->key_index, 0, &f->key[0]);
	CALL(f, send_ploam, &m, 3);
	hh_ploam_encode_key(&m, f->onu_id, f->key_index, 1, &f->key[8]);
	CALL(f, send_ploam, &m, 3);
}

/* O5-only management messages. Returns true if handled. */
static bool handle_o5(struct hh_gpon_fsm *f, const struct hh_ploam_raw *m,
                      const struct hh_ploam_ds *d)
{
	struct hh_ploam_raw r;

	switch (d->msg_id) {
	case HH_DS_REQUEST_PASSWORD:
		hh_ploam_encode_password(&r, f->onu_id, f->cfg.password);
		CALL(f, send_ploam, &r, 3);
		return true;
	case HH_DS_REQUEST_KEY:
		send_key(f);
		return true;
	case HH_DS_KEY_SWITCHING_TIME:
		CALL(f, key_switch, d->u.key_switch.superframe);
		ack(f, m);
		return true;
	case HH_DS_ENCRYPTED_PORT_ID:
		CALL(f, port_encrypt, d->u.port.port_id, d->u.port.flag);
		ack(f, m);
		return true;
	case HH_DS_CONFIGURE_PORT_ID:
		CALL(f, omci_port, d->u.port.port_id, d->u.port.flag);
		ack(f, m);
		return true;
	case HH_DS_ASSIGN_ALLOC_ID:
		if (d->u.alloc.type == HH_ALLOC_TYPE_DEALLOCATE)
			CALL(f, alloc_id, d->u.alloc.alloc_id, false);
		else if (d->u.alloc.type == HH_ALLOC_TYPE_GEM)
			CALL(f, alloc_id, d->u.alloc.alloc_id, true);
		ack(f, m);
		return true;
	case HH_DS_BER_INTERVAL:
		CALL(f, ber_interval, d->u.ber.frames);
		ack(f, m);
		return true;
	default:
		return false;
	}
}

void hh_gpon_fsm_ploam(struct hh_gpon_fsm *f, const struct hh_ploam_raw *m)
{
	struct hh_ploam_ds d;
	bool bcast = m->onu_id == HH_ONU_ID_BROADCAST;

	f->ploam_rx++;
	if (!hh_ploam_for_us(m, f->onu_id, f->have_onu_id)) {
		f->ploam_ignored++;
		return;
	}
	if (hh_ploam_decode_ds(m, &d)) {
		f->ploam_bad++;
		return;
	}

	switch (d.msg_id) {
	case HH_DS_UPSTREAM_OVERHEAD:
		if (f->state == HH_O2_STANDBY &&
		    (!f->ops->set_overhead || f->ops->set_overhead(f->ctx, &d)))
			enter(f, HH_O3_SERIAL_NUMBER);
		return;
	case HH_DS_EXT_BURST_LENGTH:
		if (f->state == HH_O3_SERIAL_NUMBER)
			CALL(f, set_ext_burst, d.u.ext_burst.t3_preranged, d.u.ext_burst.t3_ranged);
		return;
	case HH_DS_ASSIGN_ONU_ID:
		if (f->state == HH_O3_SERIAL_NUMBER && sn_match(f, d.u.assign_onu_id.sn)) {
			f->onu_id = d.u.assign_onu_id.onu_id;
			f->have_onu_id = true;
			CALL(f, set_onu_id, f->onu_id, true);
			enter(f, HH_O4_RANGING);
		}
		return;
	case HH_DS_RANGING_TIME:
		if (bcast || d.u.ranging_time.protection_path)
			return;
		if (f->state == HH_O4_RANGING) {
			CALL(f, set_eqd, d.u.ranging_time.eqd_bits);
			enter(f, HH_O5_OPERATION);
		} else if (f->state == HH_O5_OPERATION) {
			CALL(f, set_eqd, d.u.ranging_time.eqd_bits);
		}
		return;
	case HH_DS_DEACTIVATE_ONU_ID:
		if (f->state == HH_O4_RANGING || f->state == HH_O5_OPERATION ||
		    f->state == HH_O6_POPUP)
			enter(f, HH_O2_STANDBY);
		return;
	case HH_DS_DISABLE_SN: {
		uint8_t c = d.u.disable_sn.control;
		bool mine = sn_match(f, d.u.disable_sn.sn);

		if ((c == HH_DISABLE_SN_DISABLE && mine) || c == HH_DISABLE_SN_DISABLE_ALL) {
			if (f->state >= HH_O2_STANDBY && f->state <= HH_O6_POPUP)
				enter(f, HH_O7_EMERGENCY_STOP);
		} else if ((c == HH_DISABLE_SN_ENABLE && mine) || c == HH_DISABLE_SN_ENABLE_ALL) {
			if (f->state == HH_O7_EMERGENCY_STOP)
				enter(f, f->ds_synced ? HH_O2_STANDBY : HH_O1_INITIAL);
		}
		return;
	}
	case HH_DS_POPUP:
		if (f->state == HH_O6_POPUP)
			enter(f, bcast ? HH_O4_RANGING : HH_O5_OPERATION);
		return;
	case HH_DS_SWIFT_POPUP:
		if (f->state == HH_O6_POPUP && !bcast)
			enter(f, HH_O5_OPERATION);
		return;
	default:
		break;
	}

	if (f->state == HH_O5_OPERATION && handle_o5(f, m, &d))
		return;
	f->ploam_ignored++;
}
