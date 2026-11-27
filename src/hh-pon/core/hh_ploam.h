/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hh_ploam.h - ITU-T G.984.3 (GPON) PLOAM message codec.
 *
 * Pure C, no kernel or hardware dependency: builds in the kernel (hh-pon.ko)
 * and on the host (tests/). Field layouts follow G.984.3 (01/2014) clause 9.2.
 *
 * Wire format of one PLOAM (13 octets on the wire):
 *   octet 1      ONU-ID (0xFF = broadcast)
 *   octet 2      Message-ID
 *   octets 3-12  10 data octets
 *   octet 13     CRC-8 (x^8 + x^2 + x + 1)
 *
 * On BCM6858 the GPON RX MAC checks the CRC and the Runner delivers the first
 * 12 octets to CPU RX ring 6 (docs/14 section 4); the GPON TX MAC adds the CRC
 * upstream (docs/13). hh_ploam_crc8() exists for tests and for a software path.
 */
#ifndef HH_PLOAM_H
#define HH_PLOAM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#define HH_PLOAM_LEN 12 /* ONU-ID + Msg-ID + 10 data, without CRC */
#define HH_PLOAM_DATA_LEN 10
#define HH_ONU_ID_BROADCAST 0xFF
#define HH_ONU_ID_MAX 253 /* 0..253 assignable, 254 = SN-request alloc */
#define HH_ALLOC_ID_SN_REQUEST 254

/* Downstream (OLT -> ONU) message IDs, G.984.3 Table 9-1 [standard]. */
enum hh_ploam_ds_id {
	HH_DS_UPSTREAM_OVERHEAD = 1,
	HH_DS_SN_MASK = 2,
	HH_DS_ASSIGN_ONU_ID = 3,
	HH_DS_RANGING_TIME = 4,
	HH_DS_DEACTIVATE_ONU_ID = 5,
	HH_DS_DISABLE_SN = 6,
	HH_DS_ENCRYPTED_PORT_ID = 8,
	HH_DS_REQUEST_PASSWORD = 9,
	HH_DS_ASSIGN_ALLOC_ID = 10,
	HH_DS_NO_MESSAGE = 11,
	HH_DS_POPUP = 12,
	HH_DS_REQUEST_KEY = 13,
	HH_DS_CONFIGURE_PORT_ID = 14,
	HH_DS_PEE = 15,
	HH_DS_CHANGE_POWER_LEVEL = 16,
	HH_DS_PST = 17,
	HH_DS_BER_INTERVAL = 18,
	HH_DS_KEY_SWITCHING_TIME = 19,
	HH_DS_EXT_BURST_LENGTH = 20,
	HH_DS_PON_ID = 21,
	HH_DS_SWIFT_POPUP = 22,
	HH_DS_RANGING_ADJUSTMENT = 23,
	HH_DS_SLEEP_ALLOW = 24,
};

/* Upstream (ONU -> OLT) message IDs, G.984.3 Table 9-2 [standard]. */
enum hh_ploam_us_id {
	HH_US_SERIAL_NUMBER_ONU = 1,
	HH_US_PASSWORD = 2,
	HH_US_DYING_GASP = 3,
	HH_US_NO_MESSAGE = 4,
	HH_US_ENCRYPTION_KEY = 5,
	HH_US_PEE = 6,
	HH_US_PST = 7,
	HH_US_REI = 8,
	HH_US_ACKNOWLEDGE = 9,
	HH_US_SLEEP_REQUEST = 10,
};

/* Disable_Serial_Number octet 3 values. */
#define HH_DISABLE_SN_DISABLE 0xFF     /* this SN -> O7 */
#define HH_DISABLE_SN_DISABLE_ALL 0xF0 /* every ONU -> O7 */
#define HH_DISABLE_SN_ENABLE 0x00      /* this SN, O7 -> O2 */
#define HH_DISABLE_SN_ENABLE_ALL 0x0F  /* every ONU, O7 -> O2 */

/* Assign_Alloc-ID octet 5 values. */
#define HH_ALLOC_TYPE_GEM 1
#define HH_ALLOC_TYPE_DEALLOCATE 255

struct hh_ploam_raw {
	uint8_t onu_id;
	uint8_t msg_id;
	uint8_t data[HH_PLOAM_DATA_LEN];
};

/* Decoded downstream message. Only the fields of the active type are valid. */
struct hh_ploam_ds {
	uint8_t onu_id;
	uint8_t msg_id;
	union {
		struct { /* Upstream_Overhead */
			uint8_t guard_bits;
			uint8_t t1_preamble_bits;
			uint8_t t2_preamble_bits;
			uint8_t t3_pattern;
			uint8_t delimiter[3];
			uint8_t options;            /* octet 10: SN power level, EqD mode... */
			uint16_t preassigned_delay; /* 32-byte units (octets 11-12) */
		} overhead;
		struct { /* Assign_ONU-ID */
			uint8_t onu_id;
			uint8_t sn[8];
		} assign_onu_id;
		struct { /* Ranging_Time */
			bool protection_path;
			uint32_t eqd_bits;
		} ranging_time;
		struct { /* Disable_Serial_Number */
			uint8_t control;
			uint8_t sn[8];
		} disable_sn;
		struct {                  /* Encrypted_Port-ID / Configure_Port-ID */
			bool flag;        /* encrypt / activate */
			uint16_t port_id; /* 12 bits */
		} port;
		struct {                   /* Assign_Alloc-ID */
			uint16_t alloc_id; /* 12 bits */
			uint8_t type;
		} alloc;
		struct { /* BER_Interval */
			uint32_t frames;
		} ber;
		struct {                     /* Key_Switching_Time */
			uint32_t superframe; /* 30-bit superframe counter */
		} key_switch;
		struct { /* Extended_Burst_Length */
			uint8_t t3_preranged;
			uint8_t t3_ranged;
		} ext_burst;
	} u;
};

uint8_t hh_ploam_crc8(const uint8_t *buf, unsigned int len);

/* Returns 0, or -1 if the message is malformed for its type. Unknown message
 * IDs decode successfully with only onu_id/msg_id set (callers ignore them). */
int hh_ploam_decode_ds(const struct hh_ploam_raw *raw, struct hh_ploam_ds *out);

/* Upstream encoders. All fill the whole 12-octet message (unused octets 0). */
void hh_ploam_encode_sn_onu(struct hh_ploam_raw *m, uint8_t onu_id, const uint8_t sn[8],
                            uint16_t random_delay_32b, uint8_t flags);
void hh_ploam_encode_password(struct hh_ploam_raw *m, uint8_t onu_id, const uint8_t pw[10]);
void hh_ploam_encode_dying_gasp(struct hh_ploam_raw *m, uint8_t onu_id);
void hh_ploam_encode_key(struct hh_ploam_raw *m, uint8_t onu_id, uint8_t key_index,
                         uint8_t fragment, const uint8_t key_frag[8]);
void hh_ploam_encode_rei(struct hh_ploam_raw *m, uint8_t onu_id, uint32_t bip_errors, uint8_t seq);
/* Acknowledge copies the DS Message-ID and the first 9 octets of the DS message. */
void hh_ploam_encode_ack(struct hh_ploam_raw *m, uint8_t onu_id, const struct hh_ploam_raw *ds);

/* True if a downstream message is addressed to us (ONU-ID match or broadcast). */
static inline bool hh_ploam_for_us(const struct hh_ploam_raw *m, uint8_t our_id, bool have_id)
{
	return m->onu_id == HH_ONU_ID_BROADCAST || (have_id && m->onu_id == our_id);
}

#endif /* HH_PLOAM_H */
