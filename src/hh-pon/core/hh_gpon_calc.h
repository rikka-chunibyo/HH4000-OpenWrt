/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hh_gpon_calc.h - pure arithmetic of the BCM6858 GPON TX MAC (docs/13 5.4, 5.5).
 * Host-tested in tests/test_main.c.
 */
#ifndef HH_GPON_CALC_H
#define HH_GPON_CALC_H

#include "hh_ploam.h"

#define HH_GPON_US_BITS_PER_FRAME	155520u	/* 1.24416 Gb/s * 125 us */

/* ONU minimum response time in bits: stock link param 35 us -> 43545 bits. */
static inline uint32_t hh_min_resp_bits(uint32_t min_resp_us)
{
	return (uint32_t)((uint64_t)min_resp_us * 1244160u / 1000u);
}

/* TDLY register value for a total delay in bits:
 *   FDLY[18:16] = frames, BDLY[14:0] = bytes, SDLY[21:19] = bits.
 * Stock example: EqD 0x1234 + 43545 = 48205 bits -> 0x00281789. */
static inline uint32_t hh_tdly_encode(uint32_t total_bits)
{
	uint32_t frames = total_bits / HH_GPON_US_BITS_PER_FRAME;
	uint32_t rem = total_bits % HH_GPON_US_BITS_PER_FRAME;

	return ((frames & 7u) << 16) | ((rem >> 3) & 0x7fffu) | ((total_bits & 7u) << 19);
}

/* Total delay before Ranging_Time: preassigned EqD (Upstream_Overhead octets
 * 11-12, units of 256 bits, only if options bit5 "pre-equalisation" is set)
 * plus, in O3, the random delay (units of 256 bits). */
static inline uint32_t hh_preassigned_bits(uint32_t min_resp_bits, bool pre_eq,
					   uint16_t preassigned, bool sn_state, uint8_t random_delay)
{
	return min_resp_bits + (pre_eq ? (uint32_t)preassigned * 256u : 0u) +
	       (sn_state ? (uint32_t)random_delay << 8 : 0u);
}

/* Stock random delay: ((rnd & 0xffff) % 0xe947) >> 8, i.e. 0..233 (~48 us max). */
static inline uint8_t hh_random_delay(uint32_t rnd)
{
	return (uint8_t)(((rnd & 0xffffu) % 0xe947u) >> 8);
}

#define HH_OVH_OPT_PRE_EQ	0x20	/* options octet bit5 */
#define HH_OVH_OPT_SN_MASK	0x10
#define HH_OVH_OPT_EXTRA_SN(o)	(((o) >> 2) & 3)
#define HH_OVH_OPT_POWER(o)	((o) & 3)

/* Stock validity check on Upstream_Overhead: T1+T2 <= 128 and (T1+T2+GT) % 8 == 0. */
static inline bool hh_overhead_valid(uint8_t guard, uint8_t t1, uint8_t t2)
{
	return (unsigned int)t1 + t2 <= 128u && ((unsigned int)t1 + t2 + guard) % 8u == 0;
}

/* Ranging PLOAM buffer content (RPLM[0..12]): SN_ONU incl. CRC-8 (docs/13 5.5). */
static inline void hh_build_rplm(uint8_t out[13], uint8_t onu_id, const uint8_t sn[8],
				 uint8_t random_delay, uint8_t power_level)
{
	struct hh_ploam_raw m;
	unsigned int i;

	/* 12-bit random delay field; stock keeps the low nibble for GEM/power flags */
	hh_ploam_encode_sn_onu(&m, onu_id, sn, random_delay, 0x4 | (power_level & 3));
	out[0] = m.onu_id;
	out[1] = m.msg_id;
	for (i = 0; i < HH_PLOAM_DATA_LEN; i++)
		out[2 + i] = m.data[i];
	out[12] = hh_ploam_crc8(out, 12);
}

#endif /* HH_GPON_CALC_H */
