// SPDX-License-Identifier: GPL-2.0-only
/*
 * hh_ploam.c - G.984.3 PLOAM codec (see hh_ploam.h).
 */
#include "hh_ploam.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* CRC-8, generator x^8 + x^2 + x + 1 (0x07), init 0, no reflection (G.984.3 9.1). */
uint8_t hh_ploam_crc8(const uint8_t *buf, unsigned int len)
{
	uint8_t crc = 0;
	unsigned int i, b;

	for (i = 0; i < len; i++) {
		crc ^= buf[i];
		for (b = 0; b < 8; b++)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
	}
	return crc;
}

int hh_ploam_decode_ds(const struct hh_ploam_raw *raw, struct hh_ploam_ds *out)
{
	const uint8_t *d = raw->data;	/* d[0] is octet 3 */

	memset(out, 0, sizeof(*out));
	out->onu_id = raw->onu_id;
	out->msg_id = raw->msg_id;

	switch (raw->msg_id) {
	case HH_DS_UPSTREAM_OVERHEAD:
		out->u.overhead.guard_bits = d[0];
		out->u.overhead.t1_preamble_bits = d[1];
		out->u.overhead.t2_preamble_bits = d[2];
		out->u.overhead.t3_pattern = d[3];
		memcpy(out->u.overhead.delimiter, &d[4], 3);
		out->u.overhead.options = d[7];
		out->u.overhead.preassigned_delay = be16(&d[8]);
		/* Upstream_Overhead is always broadcast. */
		if (raw->onu_id != HH_ONU_ID_BROADCAST)
			return -1;
		break;
	case HH_DS_ASSIGN_ONU_ID:
		out->u.assign_onu_id.onu_id = d[0];
		memcpy(out->u.assign_onu_id.sn, &d[1], 8);
		if (d[0] > HH_ONU_ID_MAX)
			return -1;
		break;
	case HH_DS_RANGING_TIME:
		out->u.ranging_time.protection_path = d[0] & 1;
		out->u.ranging_time.eqd_bits = be32(&d[1]);
		break;
	case HH_DS_DISABLE_SN:
		out->u.disable_sn.control = d[0];
		memcpy(out->u.disable_sn.sn, &d[1], 8);
		break;
	case HH_DS_ENCRYPTED_PORT_ID:
	case HH_DS_CONFIGURE_PORT_ID:
		out->u.port.flag = d[0] & 1;
		out->u.port.port_id = be16(&d[1]) >> 4;	/* 12 MSBs of octets 4-5 */
		break;
	case HH_DS_ASSIGN_ALLOC_ID:
		out->u.alloc.alloc_id = be16(&d[0]) >> 4;
		out->u.alloc.type = d[2];
		break;
	case HH_DS_BER_INTERVAL:
		out->u.ber.frames = be32(&d[0]);
		break;
	case HH_DS_KEY_SWITCHING_TIME:
		out->u.key_switch.superframe = be32(&d[0]) & 0x3fffffff;
		break;
	case HH_DS_EXT_BURST_LENGTH:
		out->u.ext_burst.t3_preranged = d[0];
		out->u.ext_burst.t3_ranged = d[1];
		break;
	default:
		break;
	}
	return 0;
}

static void us_init(struct hh_ploam_raw *m, uint8_t onu_id, uint8_t id)
{
	memset(m, 0, sizeof(*m));
	m->onu_id = onu_id;
	m->msg_id = id;
}

void hh_ploam_encode_sn_onu(struct hh_ploam_raw *m, uint8_t onu_id, const uint8_t sn[8],
			    uint16_t random_delay_32b, uint8_t flags)
{
	us_init(m, onu_id, HH_US_SERIAL_NUMBER_ONU);
	memcpy(m->data, sn, 8);			/* Vendor-ID (4) + VSSN (4) */
	m->data[8] = (uint8_t)(random_delay_32b >> 4);
	m->data[9] = (uint8_t)((random_delay_32b & 0xf) << 4) | (flags & 0x0f);
}

void hh_ploam_encode_password(struct hh_ploam_raw *m, uint8_t onu_id, const uint8_t pw[10])
{
	us_init(m, onu_id, HH_US_PASSWORD);
	memcpy(m->data, pw, 10);
}

void hh_ploam_encode_dying_gasp(struct hh_ploam_raw *m, uint8_t onu_id)
{
	us_init(m, onu_id, HH_US_DYING_GASP);
}

void hh_ploam_encode_key(struct hh_ploam_raw *m, uint8_t onu_id, uint8_t key_index,
			 uint8_t fragment, const uint8_t key_frag[8])
{
	us_init(m, onu_id, HH_US_ENCRYPTION_KEY);
	m->data[0] = key_index;
	m->data[1] = fragment;
	memcpy(&m->data[2], key_frag, 8);
}

void hh_ploam_encode_rei(struct hh_ploam_raw *m, uint8_t onu_id, uint32_t bip_errors, uint8_t seq)
{
	us_init(m, onu_id, HH_US_REI);
	put_be32(&m->data[0], bip_errors);
	m->data[4] = seq & 0x0f;
}

void hh_ploam_encode_ack(struct hh_ploam_raw *m, uint8_t onu_id, const struct hh_ploam_raw *ds)
{
	us_init(m, onu_id, HH_US_ACKNOWLEDGE);
	m->data[0] = ds->msg_id;
	/* DM octets 1..9 of the downstream message: ONU-ID, Msg-ID, data[0..6]. */
	m->data[1] = ds->onu_id;
	m->data[2] = ds->msg_id;
	memcpy(&m->data[3], ds->data, 7);
}
