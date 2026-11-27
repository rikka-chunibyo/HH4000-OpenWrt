// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host unit tests for the PLOAM codec and the O1..O7 state machine.
 * Build and run: make -C src/hh-pon test
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "../core/hh_ploam.h"
#include "../core/hh_gpon_fsm.h"
#include "../core/hh_gpon_calc.h"

static int fails, checks;
#define CHECK(c)                                                                                   \
	do {                                                                                       \
		checks++;                                                                          \
		if (!(c)) {                                                                        \
			fails++;                                                                   \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
		}                                                                                  \
	} while (0)

/* ---------- recorder ops ---------- */
struct rec {
	char log[4096];
	struct hh_ploam_raw sent[16];
	unsigned int sent_repeat[16];
	int nsent;
	bool laser, link;
	uint8_t onu_id;
	bool onu_valid;
	uint32_t eqd;
	int to1, to2; /* running? */
	uint16_t alloc;
	bool alloc_on;
	uint16_t omci_port;
	bool omci_on;
};

static void logf_(struct rec *r, const char *fmt, ...)
{
	va_list ap;
	size_t n = strlen(r->log);
	va_start(ap, fmt);
	vsnprintf(r->log + n, sizeof(r->log) - n, fmt, ap);
	va_end(ap);
}
static void o_state(void *c, enum hh_gpon_state s)
{
	logf_(c, "S%d ", s);
}
static void o_onu(void *c, uint8_t id, bool v)
{
	struct rec *r = c;
	r->onu_id = id;
	r->onu_valid = v;
}
static bool o_ovh(void *c, const struct hh_ploam_ds *d)
{
	logf_(c, "OVH ");
	return d->u.overhead.t1_preamble_bits + d->u.overhead.t2_preamble_bits <= 128;
}
static void o_eqd(void *c, uint32_t e)
{
	((struct rec *)c)->eqd = e;
}
static void o_laser(void *c, bool on)
{
	((struct rec *)c)->laser = on;
}
static void o_send(void *c, const struct hh_ploam_raw *m, unsigned int rep)
{
	struct rec *r = c;
	if (r->nsent < 16) {
		r->sent[r->nsent] = *m;
		r->sent_repeat[r->nsent] = rep;
		r->nsent++;
	}
}
static void o_tstart(void *c, enum hh_gpon_timer t, unsigned int ms)
{
	struct rec *r = c;
	(void)ms;
	if (t == HH_TIMER_TO1)
		r->to1 = 1;
	else
		r->to2 = 1;
}
static void o_tstop(void *c, enum hh_gpon_timer t)
{
	struct rec *r = c;
	if (t == HH_TIMER_TO1)
		r->to1 = 0;
	else
		r->to2 = 0;
}
static void o_alloc(void *c, uint16_t a, bool on)
{
	struct rec *r = c;
	r->alloc = a;
	r->alloc_on = on;
}
static void o_omci(void *c, uint16_t p, bool on)
{
	struct rec *r = c;
	r->omci_port = p;
	r->omci_on = on;
}
static void o_keygen(void *c, uint8_t k[16])
{
	(void)c;
	for (int i = 0; i < 16; i++)
		k[i] = (uint8_t)(0xA0 + i);
}
static void o_link(void *c, bool up)
{
	((struct rec *)c)->link = up;
}

static const struct hh_gpon_ops rec_ops = {
        .set_state = o_state,
        .set_onu_id = o_onu,
        .set_overhead = o_ovh,
        .set_eqd = o_eqd,
        .laser_enable = o_laser,
        .send_ploam = o_send,
        .timer_start = o_tstart,
        .timer_stop = o_tstop,
        .alloc_id = o_alloc,
        .omci_port = o_omci,
        .key_generate = o_keygen,
        .link = o_link,
};

static const uint8_t SN[8] = {'S', 'M', 'B', 'S', 0x12, 0x34, 0x56, 0x78}; /* test value */
static const uint8_t OTHER_SN[8] = {'H', 'W', 'T', 'C', 1, 2, 3, 4};

static struct hh_ploam_raw ds(uint8_t onu, uint8_t id, const uint8_t *data, int n)
{
	struct hh_ploam_raw m;
	memset(&m, 0, sizeof(m));
	m.onu_id = onu;
	m.msg_id = id;
	if (data)
		memcpy(m.data, data, n);
	return m;
}

static void setup(struct hh_gpon_fsm *f, struct rec *r)
{
	struct hh_gpon_fsm_cfg cfg = {0};
	memcpy(cfg.sn, SN, 8);
	memcpy(cfg.password, "0123456789", 10);
	memset(r, 0, sizeof(*r));
	hh_gpon_fsm_init(f, &cfg, &rec_ops, r);
}

/* Drive a normal activation to O5 with ONU-ID 5. */
static void activate_to_o5(struct hh_gpon_fsm *f, struct rec *r)
{
	uint8_t ovh[10] = {32, 44, 0, 0xAA, 0xAB, 0x59, 0x83, 0, 0, 0};
	uint8_t aid[9];
	aid[0] = 5;
	memcpy(&aid[1], SN, 8);
	uint8_t rt[5] = {0, 0x00, 0x01, 0x23, 0x45};
	struct hh_ploam_raw m;

	hh_gpon_fsm_signal(f, HH_SIG_ACTIVATE);
	hh_gpon_fsm_signal(f, HH_SIG_DS_SYNC);
	CHECK(f->state == HH_O2_STANDBY);
	m = ds(0xFF, HH_DS_UPSTREAM_OVERHEAD, ovh, 10);
	hh_gpon_fsm_ploam(f, &m);
	CHECK(f->state == HH_O3_SERIAL_NUMBER);
	CHECK(r->laser);
	m = ds(0xFF, HH_DS_ASSIGN_ONU_ID, aid, 9);
	hh_gpon_fsm_ploam(f, &m);
	CHECK(f->state == HH_O4_RANGING);
	CHECK(r->onu_valid && r->onu_id == 5);
	CHECK(r->to1 == 1);
	m = ds(5, HH_DS_RANGING_TIME, rt, 5);
	hh_gpon_fsm_ploam(f, &m);
	CHECK(f->state == HH_O5_OPERATION);
	CHECK(r->eqd == 0x00012345);
	CHECK(r->to1 == 0);
	CHECK(r->link);
}

/* ---------- codec tests ---------- */
static void test_codec(void)
{
	struct hh_ploam_raw m;
	struct hh_ploam_ds d;
	uint8_t aid[9] = {7, 'A', 'B', 'C', 'D', 1, 2, 3, 4};
	uint8_t alloc[3] = {0x12, 0x30, 1}; /* Alloc-ID 0x123, GEM */
	uint8_t port[3] = {1, 0xFF, 0xF0};  /* activate, Port-ID 0xFFF */
	uint8_t ks[4] = {0xC0, 0, 0x10, 0}; /* top bits must be masked */

	m = ds(0xFF, HH_DS_ASSIGN_ONU_ID, aid, 9);
	CHECK(hh_ploam_decode_ds(&m, &d) == 0 && d.u.assign_onu_id.onu_id == 7 &&
	      memcmp(d.u.assign_onu_id.sn, "ABCD", 4) == 0);
	aid[0] = 254;
	m = ds(0xFF, HH_DS_ASSIGN_ONU_ID, aid, 9);
	CHECK(hh_ploam_decode_ds(&m, &d) == -1);

	m = ds(3, HH_DS_ASSIGN_ALLOC_ID, alloc, 3);
	CHECK(hh_ploam_decode_ds(&m, &d) == 0 && d.u.alloc.alloc_id == 0x123 &&
	      d.u.alloc.type == 1);
	m = ds(3, HH_DS_CONFIGURE_PORT_ID, port, 3);
	CHECK(hh_ploam_decode_ds(&m, &d) == 0 && d.u.port.flag && d.u.port.port_id == 0xFFF);
	m = ds(3, HH_DS_KEY_SWITCHING_TIME, ks, 4);
	CHECK(hh_ploam_decode_ds(&m, &d) == 0 && d.u.key_switch.superframe == 0x00001000);

	/* Upstream_Overhead must be broadcast */
	m = ds(3, HH_DS_UPSTREAM_OVERHEAD, NULL, 0);
	CHECK(hh_ploam_decode_ds(&m, &d) == -1);

	/* Ack layout: DM_ID then the first 9 DS octets */
	struct hh_ploam_raw a;
	m = ds(3, HH_DS_ASSIGN_ALLOC_ID, alloc, 3);
	hh_ploam_encode_ack(&a, 3, &m);
	CHECK(a.msg_id == HH_US_ACKNOWLEDGE && a.data[0] == HH_DS_ASSIGN_ALLOC_ID &&
	      a.data[1] == 3 && a.data[2] == HH_DS_ASSIGN_ALLOC_ID && a.data[3] == 0x12 &&
	      a.data[4] == 0x30 && a.data[5] == 1);

	/* CRC-8 (poly 0x07): check value for "123456789" is 0xF4 */
	CHECK(hh_ploam_crc8((const uint8_t *)"123456789", 9) == 0xF4);

	hh_ploam_encode_rei(&a, 9, 0x01020304, 0x1F);
	CHECK(a.data[0] == 1 && a.data[3] == 4 && a.data[4] == 0x0F);
}

/* ---------- FSM tests ---------- */
static void test_activation(void)
{
	struct hh_gpon_fsm f;
	struct rec r;
	setup(&f, &r);
	CHECK(f.state == HH_O1_INITIAL && !r.laser);
	hh_gpon_fsm_signal(&f, HH_SIG_DS_SYNC);
	CHECK(f.state == HH_O1_INITIAL); /* admin down until ACTIVATE */
	activate_to_o5(&f, &r);
}

static void test_wrong_sn_and_addressing(void)
{
	struct hh_gpon_fsm f;
	struct rec r;
	struct hh_ploam_raw m;
	uint8_t aid[9];
	aid[0] = 9;
	memcpy(&aid[1], OTHER_SN, 8);
	uint8_t ovh[10] = {0};

	setup(&f, &r);
	hh_gpon_fsm_signal(&f, HH_SIG_ACTIVATE);
	hh_gpon_fsm_signal(&f, HH_SIG_DS_SYNC);
	m = ds(0xFF, HH_DS_UPSTREAM_OVERHEAD, ovh, 10);
	hh_gpon_fsm_ploam(&f, &m);
	m = ds(0xFF, HH_DS_ASSIGN_ONU_ID, aid, 9);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O3_SERIAL_NUMBER && !f.have_onu_id);
	/* A directed message to ONU 0 must be ignored while we hold no ID. */
	m = ds(0, HH_DS_DEACTIVATE_ONU_ID, NULL, 0);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O3_SERIAL_NUMBER);

	setup(&f, &r);
	activate_to_o5(&f, &r);
	m = ds(6, HH_DS_DEACTIVATE_ONU_ID, NULL, 0);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O5_OPERATION); /* other ONU */
	m = ds(0xFF, HH_DS_DEACTIVATE_ONU_ID, NULL, 0);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O2_STANDBY && !r.onu_valid && !r.laser && !r.link);
}

static void test_to1_and_los(void)
{
	struct hh_gpon_fsm f;
	struct rec r;
	struct hh_ploam_raw m;
	uint8_t aid[9];
	aid[0] = 5;
	memcpy(&aid[1], SN, 8);
	uint8_t ovh[10] = {0};

	setup(&f, &r);
	hh_gpon_fsm_signal(&f, HH_SIG_ACTIVATE);
	hh_gpon_fsm_signal(&f, HH_SIG_DS_SYNC);
	m = ds(0xFF, HH_DS_UPSTREAM_OVERHEAD, ovh, 10);
	hh_gpon_fsm_ploam(&f, &m);
	m = ds(0xFF, HH_DS_ASSIGN_ONU_ID, aid, 9);
	hh_gpon_fsm_ploam(&f, &m);
	hh_gpon_fsm_signal(&f, HH_SIG_TO1_EXPIRED);
	CHECK(f.state == HH_O2_STANDBY && !f.have_onu_id);

	setup(&f, &r);
	activate_to_o5(&f, &r);
	hh_gpon_fsm_signal(&f, HH_SIG_LOS_LOF);
	CHECK(f.state == HH_O6_POPUP && r.to2 && !r.laser && !r.link && f.have_onu_id);
	/* directed POPUP -> O5, keeps ONU-ID */
	m = ds(5, HH_DS_POPUP, NULL, 0);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O5_OPERATION && r.to2 == 0 && r.laser);
	/* broadcast POPUP -> O4 */
	hh_gpon_fsm_signal(&f, HH_SIG_LOS_LOF);
	m = ds(0xFF, HH_DS_POPUP, NULL, 0);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O4_RANGING && r.to1);
	/* TO2 expiry -> O1 */
	setup(&f, &r);
	activate_to_o5(&f, &r);
	hh_gpon_fsm_signal(&f, HH_SIG_LOS_LOF);
	hh_gpon_fsm_signal(&f, HH_SIG_TO2_EXPIRED);
	CHECK(f.state == HH_O1_INITIAL && !f.have_onu_id);
	hh_gpon_fsm_signal(&f, HH_SIG_DS_SYNC);
	CHECK(f.state == HH_O2_STANDBY);
}

static void test_emergency_stop(void)
{
	struct hh_gpon_fsm f;
	struct rec r;
	struct hh_ploam_raw m;
	uint8_t dis[9];
	dis[0] = HH_DISABLE_SN_DISABLE;
	memcpy(&dis[1], SN, 8);

	setup(&f, &r);
	activate_to_o5(&f, &r);
	dis[1] ^= 1;
	m = ds(0xFF, HH_DS_DISABLE_SN, dis, 9);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O5_OPERATION); /* not our SN */
	dis[1] ^= 1;
	m = ds(0xFF, HH_DS_DISABLE_SN, dis, 9);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O7_EMERGENCY_STOP && !r.laser);
	hh_gpon_fsm_signal(&f, HH_SIG_LOS_LOF);
	hh_gpon_fsm_signal(&f, HH_SIG_DS_SYNC);
	CHECK(f.state == HH_O7_EMERGENCY_STOP); /* sticky */
	dis[0] = HH_DISABLE_SN_ENABLE;
	m = ds(0xFF, HH_DS_DISABLE_SN, dis, 9);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O2_STANDBY);
}

static void test_o5_management(void)
{
	struct hh_gpon_fsm f;
	struct rec r;
	struct hh_ploam_raw m;
	uint8_t alloc[3] = {0x40, 0x10, 1}; /* Alloc-ID 0x401 */
	uint8_t port[3] = {1, 0x40, 0x10};  /* OMCI port 0x401 */

	setup(&f, &r);
	activate_to_o5(&f, &r);
	m = ds(5, HH_DS_ASSIGN_ALLOC_ID, alloc, 3);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(r.alloc == 0x401 && r.alloc_on);
	CHECK(r.nsent == 1 && r.sent[0].msg_id == HH_US_ACKNOWLEDGE && r.sent[0].onu_id == 5);
	m = ds(5, HH_DS_CONFIGURE_PORT_ID, port, 3);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(r.omci_port == 0x401 && r.omci_on && r.nsent == 2);
	m = ds(5, HH_DS_REQUEST_PASSWORD, NULL, 0);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(r.nsent == 3 && r.sent[2].msg_id == HH_US_PASSWORD && r.sent_repeat[2] == 3 &&
	      memcmp(r.sent[2].data, "0123456789", 10) == 0);
	m = ds(5, HH_DS_REQUEST_KEY, NULL, 0);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(r.nsent == 5 && r.sent[3].msg_id == HH_US_ENCRYPTION_KEY && r.sent[3].data[1] == 0 &&
	      r.sent[4].data[1] == 1 && r.sent[4].data[2] == 0xA8);
	/* broadcast Assign_Alloc-ID: applied, not acknowledged */
	m = ds(0xFF, HH_DS_ASSIGN_ALLOC_ID, alloc, 3);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(r.nsent == 5);
}

static void test_ext_burst_only_o3(void)
{
	struct hh_gpon_fsm f;
	struct rec r;
	struct hh_ploam_raw m;
	uint8_t eb[2] = {3, 4};
	setup(&f, &r);
	activate_to_o5(&f, &r);
	unsigned int ign = f.ploam_ignored;
	m = ds(0xFF, HH_DS_EXT_BURST_LENGTH, eb, 2);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O5_OPERATION && f.ploam_ignored == ign);
}

static int rplm_calls;
static void o_rplm(void *c)
{
	(void)c;
	rplm_calls++;
}

static void test_to1_from_o3(void)
{
	struct hh_gpon_fsm f;
	struct rec r;
	struct hh_ploam_raw m;
	struct hh_gpon_ops ops = rec_ops;
	struct hh_gpon_fsm_cfg cfg = {0};
	uint8_t ovh[10] = {0};
	uint8_t aid[9];
	aid[0] = 5;
	memcpy(&aid[1], SN, 8);

	ops.sn_response_sent = o_rplm;
	memcpy(cfg.sn, SN, 8);
	memset(&r, 0, sizeof(r));
	hh_gpon_fsm_init(&f, &cfg, &ops, &r);
	CHECK(f.cfg.to1_ms == 20000);
	hh_gpon_fsm_signal(&f, HH_SIG_ACTIVATE);
	hh_gpon_fsm_signal(&f, HH_SIG_DS_SYNC);
	m = ds(0xFF, HH_DS_UPSTREAM_OVERHEAD, ovh, 10);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O3_SERIAL_NUMBER && r.to1 == 1);
	hh_gpon_fsm_signal(&f, HH_SIG_RPLM_SENT);
	CHECK(rplm_calls == 1);
	hh_gpon_fsm_signal(&f, HH_SIG_TO1_EXPIRED);
	CHECK(f.state == HH_O2_STANDBY && r.to1 == 0 && !r.laser);
	/* TO1 keeps running from O3 into O4 */
	m = ds(0xFF, HH_DS_UPSTREAM_OVERHEAD, ovh, 10);
	hh_gpon_fsm_ploam(&f, &m);
	m = ds(0xFF, HH_DS_ASSIGN_ONU_ID, aid, 9);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O4_RANGING && r.to1 == 1);
	hh_gpon_fsm_signal(&f, HH_SIG_RPLM_SENT);
	CHECK(rplm_calls == 1); /* O4: ranging response, no new random delay */
}

static void test_calc(void)
{
	uint8_t b[13];
	/* docs/13 5.4: 35 us -> 43545 bits; EqD 0x1234 -> TDLY 0x00281789 */
	CHECK(hh_min_resp_bits(35) == 43545);
	CHECK(hh_tdly_encode(43545 + 0x1234) == 0x00281789);
	CHECK(hh_tdly_encode(155520 * 2 + 9) == ((2u << 16) | 1u | (1u << 19)));
	CHECK(hh_random_delay(0xffffffff) <= 233 && hh_random_delay(0) == 0);
	CHECK(hh_overhead_valid(32, 40, 64) && !hh_overhead_valid(32, 100, 40) &&
	      !hh_overhead_valid(1, 40, 64));
	hh_build_rplm(b, 0xFF, SN, 0x12, 1);
	CHECK(b[0] == 0xFF && b[1] == 1 && b[2] == (uint8_t)0x53 && b[10] == 0x01 && b[11] == 0x25);
	CHECK(hh_ploam_crc8(b, 13) == 0); /* CRC appended correctly -> residue 0 */
}

static void test_bad_overhead(void)
{
	struct hh_gpon_fsm f;
	struct rec r;
	struct hh_ploam_raw m;
	uint8_t ovh[10] = {32, 100, 40, 0}; /* T1+T2 > 128 */
	setup(&f, &r);
	hh_gpon_fsm_signal(&f, HH_SIG_ACTIVATE);
	hh_gpon_fsm_signal(&f, HH_SIG_DS_SYNC);
	m = ds(0xFF, HH_DS_UPSTREAM_OVERHEAD, ovh, 10);
	hh_gpon_fsm_ploam(&f, &m);
	CHECK(f.state == HH_O2_STANDBY && !r.laser);
}

int main(void)
{
	test_codec();
	test_activation();
	test_wrong_sn_and_addressing();
	test_to1_and_los();
	test_emergency_stop();
	test_o5_management();
	test_ext_burst_only_o3();
	test_to1_from_o3();
	test_calc();
	test_bad_overhead();
	printf("%d/%d checks passed\n", checks - fails, checks);
	return fails ? 1 : 0;
}
