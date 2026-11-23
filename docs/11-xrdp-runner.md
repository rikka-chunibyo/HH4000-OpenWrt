# 11 - XRDP Runner packet datapath (BCM6858): a spec for an open driver

Scope: what an open-source Linux (or U-Boot) driver must do to move packets between the ARM cores and
the BCM6858 Ethernet ports through the XRDP "Runner" network processor. It covers the register map
with bit fields, the block init order, Runner firmware loading, CPU RX/TX rings and descriptors,
buffer management, interrupts, the PON differences, and a plan with effort estimates.
Companion docs: [02-ethernet.md](02-ethernet.md) (ports, LPORT/MAC/PHY), 08-pon-reimplementation.md §2 (RU register database).

Paths are relative to `$P=/path/to/HH4000-OpenWRT`. Labels: **[V]** verified (file, symbol,
offset or log line cited), **[I]** inferred, **[U]** unknown/open. This is static analysis only; nothing
here has been run on the router.

---

## 0. Summary

1. **[V] GPL bit-field definitions for the 6858 XRDP exist and match the stock driver exactly.**
   ASUS-Merlin `release/src-rt-5.04behnd.4916/rdp/drivers/rdp_subsystem/BCM6858/autogen/XRDP_AG.h`
   (DUAL/GPL, SDK 5.04) has **35 blocks, 969 registers, 2,957 fields**. All 969 register names, offsets
   and RAM counts are identical to the RU tables extracted from the stock `rdpa.ko` (5.02L.07).
   Independently, a new accessor-code analyser recovered field layouts for 781 registers from the
   1,366 `ag_drv_*` functions in `rdpa.ko`. **753 of 781 agree exactly with the GPL header**, 12 are a
   strict subset, and the 16 differences are accessor artefacts (whole-word moves or several fields
   packed into one C variable), not hardware differences (§1.2). Merged output: `work/regdb/rdpa_fields.{h,json}`.
2. **[V] Broadcom ships a GPL, register-level, CPU-only datapath for exactly this chip:** U-Boot 2019.07
   "reduced XRDP" (`bootloaders/u-boot-2019.07/drivers/net/bcmbca/xrdp/`, SPDX GPL-2.0+) in the
   5.04axhnd.675x and 5.04behnd.4916 trees. `data_path_6858.c` is a **recorded log of every register
   and SRAM write** made by the full XRDP init (2,390 tuples), including a 178-word Runner core-0
   microcode. `bcmbca_xrdp_api.c` plus `rdp_drv_sbpm.c` (about 700 lines) are the complete packet
   RX/TX code. I decoded the log against the field DB (`tools/xrdp_initlog_decode.py`,
   `work/eth/xrdp/uboot_6858_init.txt`). **This is the shortest route to a first open driver:
   replay the log, then use the U-Boot RX/TX code.** §2.
3. **[V] The 5.02L.07 GPL XRDP_CFE project** (`release/src-rt-5.02L.07p2axhnd/rdp/projects/XRDP_CFE/`)
   is the source-level version of that init (same block order as the stock `rdpa.ko`), with 3 Runner
   images and CPU rings in DDR. The stock `rdpa.ko` runs the same order with 5 firmware images on
   16 cores. §3.
4. **[V] CPU interface on the stock driver:** 16-byte RX descriptors in DDR rings (queues 3/4 for
   `bcm_enet`, 7..15 for WiFi), one 8-byte **feed ring** (16) of empty host buffers, two 8-byte
   **recycle rings** (17, 18) for TX completions, and two 16-byte **CPU TX rings** (19, 20).
   Interrupts are the 32 bits of `UBUS_SLV.RNR_INTR_CTRL` (GIC SPI 128..159). Doorbells are writes of a
   thread number to `RNR_REGS[core].CFG_CPU_WAKEUP`. §4.
5. **Effort [I]:** the XRDP part alone (polling, SBPM-only, all ports, GPL only) takes **2-4 weeks** for
   someone comfortable with kernel drivers, once LPORT/PHY bring-up exists. A first usable homelab
   setup (eth0-3 + eth4 at 1G/2.5G, polled) takes about **3-5 months** including LPORT, Merlin and the
   BCM84891. An interrupt-driven DDR-ring driver at useful throughput adds **2-3 months**. Hardware
   offload or PON on top takes **many months** (§7). The concrete eth0 + eth4 bring-up order is in §2.5.

---

## 1. Register map with bit fields

### 1.1 Sources
| Source | Content | Licence |
|---|---|---|
| `rdpa.ko` RU tables → `work/regdb/rdpa.{h,json}` (`tools/ru_extract.py`, doc 08 §2) | 35 blocks, base addresses (all instances), 969 registers, offsets, RAM count/stride. No fields | proprietary binary (facts only) |
| `rdpa.ko` `ag_drv_*` accessors → `tools/ag_fields_extract.py` (this pass) | field (lsb, width) per register for 781 registers | derived from binary |
| `5.04behnd.4916/.../BCM6858/autogen/XRDP_AG.h` (copy: `work/eth/ref/xrdp_ag/BCM6858_XRDP_AG.h`) | 969 registers, 2,957 named fields (`_FIELD_MASK/WIDTH/SHIFT`), register offsets and RAM counts. No block base addresses (those are in the missing `XRDP_AG.c`) | DUAL/GPL |
| Siblings in the same tree: `BCM63158`, `BCM6856`, `BCM6846`, `BCM4912` `XRDP_AG.h` (copied to `work/eth/ref/xrdp_ag/`); full per-block `xrdp_drv_*_ag.{c,h}` exist only for `BCM6837` and `BCM6888` | cross-reference and accessor source style | DUAL/GPL |
| `5.04axhnd.675x/bootloaders/u-boot-2019.07/arch/arm/mach-bcmbca/xrdp/bcm63146/XRDP_AG.h` | 63146 (U-Boot full mode) | GPL |

No `XRDP_AG.c` or `xrdp_drv_*_ag.c` for 6858 exists anywhere in the Merlin repository (checked the full
`git ls-tree`, 494,047 paths). Base addresses must therefore come from `rdpa.ko` (already extracted), and
the 6858 accessors must be regenerated from the header (mechanical).

### 1.2 Accessor analysis (`tools/ag_fields_extract.py`)
Method [V]: every accessor reaches its register through a literal-pool pointer to `<BLK>_BLOCK`
(`R_AARCH64_ABS64`), then `ldr x,[blk,#8]` (address array), `ldr x,[array,idx,lsl #3]` (instance base),
`ldr/str w,[base,#off]` (register). The script does a small symbolic dataflow over capstone
disassembly: getters split the loaded value with `ubfx`/`and`/`lsr`+store-width/`uxtb`/`uxth`; setters
range-check inputs with `cmp wN,#(2^w-1)` (Broadcom's `VALIDATE_PARMS`) and place them with
`lsl`/`orr ..,lsl #n`/`bfi`/`ubfiz`. Read-modify-write setters (`and #~mask` then `orr`) give the field
the width of the cleared hole. Constants built with `mov`/`movk` and `add #imm,lsl #12` are tracked for
offsets.

Results (run: `python3 -I tools/ag_fields_extract.py rdpa.ko work/regdb/rdpa.json work/regdb/rdpa_fields --gpl work/eth/ref/xrdp_ag/BCM6858_XRDP_AG.h`):

| Metric | Count |
|---|---|
| accessor functions analysed | 1,366 |
| registers touched by an accessor | 856 / 969 |
| registers with accessor-derived fields | 781 |
| ... exact match with GPL fields (excluding RESERVED) | **753** |
| ... accessor fields a strict subset of GPL (setter-only registers) | 12 |
| ... differences | 16 (listed in `work/regdb/rdpa_fields_mismatch.txt`) |
| fields in output: both sources agree (AG) / GPL only (G) / accessor only (A) | 1,837 / 407 / 2 |

The 16 differences (all listed, with both field sets, in `work/regdb/rdpa_fields_mismatch.txt` since
pass 2) fall into two groups:
- the accessor moves a whole 32-bit or 16-bit C variable without a range check: `BBH_TX.DEBUG_COUNTERS_SRAMPD/DDRPKT`
  read as 16-bit, `DDRTMBASEH` and `DBGOUTREG` as 32-bit (also in the QM-embedded `QM_BBH_TX_*` copies),
  `BBH_RX.DEBUG_SBNFIFO/SBNRECYCLEFIFO`, `DSPTCHR.QDES_FBDNULL`, `FPM.POOL1/2_STAT8` split as 31+1 bits;
- the accessor packs several GPL fields into one value: `BBH_RX.GENERAL_CONFIGURATION_MINPKT0` (4 x 8-bit
  MINPKT0..3 seen as 24+8), `RNR_QUAD.PARSER_CORE_CONFIGURATION_USER_IP_PROT` (4 x 8), and
  `DSPTCHR.INGRS_QUEUES_Q_INGRS_LIMITS` (CMN_MAX 10 + GURNTD_MAX 10 seen as one 20-bit value).

Two of these, `MINPKT0` and `Q_INGRS_LIMITS`, are written on the init path (§2.2 rows 7 and 11). In both,
the GPL split is the right one: the logged `MINPKT0=0x40404040` is four 64-byte minimums. **Conclusion
[V]: the 5.04 GPL header describes the hardware the stock 5.02L.07 driver uses. Where the two sources
differ, use the GPL field split.**

### 1.3 Output format
`work/regdb/rdpa_fields.json`: `{"blocks":[{"name","bases":[...],"regs":[{"name","addr","ram_count",
"stride","fields":[{"name","lsb","width","mask","src"}],"accessors":[...]}]}],"stats":{...}}`.
`src` is `AG`, `G` or `A`. `rdpa_fields.h` has one `#define <BLK>_<REG>_OFFSET` per register and one
`#define <BLK>_<REG>_<FIELD>_MASK` per field, with width, lsb and source in a comment. RESERVED fields are
omitted.

### 1.4 Block map (6858) [V, from the RU tables]
| Block | Instances (base) | Role |
|---|---|---|
| QM | 0x82100000 (+ `XRDP_UBUS_TOP` window at +0x40000) | Queue manager: 160+ egress queues, WRED, FPM user groups, Runner groups |
| DQM | 0x8218004c | Descriptor queue memory front-end of QM |
| RNR_MEM / RNR_INST / RNR_CNTXT / RNR_PRED | 16 cores: 0x822{0,2,4,6}0000, 0x823{0,2,4,6}0000, 0x824..., 0x825... (+0x10000 INST, +0x18000 CNTXT, +0x1c000 PRED) | per core: data SRAM (16 KiB used by fw tables), instruction RAM (32 KiB = 8192 words), thread contexts, branch prediction |
| RNR_REGS | 0x8228{0,1,2,3}000, 0x8238x000, 0x8248x000, 0x8258x000 | per core control: `CFG_GLOBAL_CTRL` (EN, MICRO_SEC_VAL), `CFG_CPU_WAKEUP` (+4), `CFG_INT_CTRL/MASK`, `CFG_DDR_CFG`, `CFG_PSRAM_CFG`, `CFG_SCH_CFG`, `RNR_FREQ` |
| RNR_QUAD | 0x82288000, 0x82388000, 0x82488000, 0x82588000 | 4 quads x 4 cores: parser, UBUS slave windows/decode, power save, DMA arbitration, flow-control tokens |
| PSRAM_MEM / PSRAM | 0x82600000 / 0x82c80000 | packet SRAM (SBPM buffers, 128 B each) and its controller |
| FPM | 0x82a00000 | Free Pool Manager: tokens for DDR packet buffers (pool in `rdp1`) |
| BBH_TX / BBH_RX | 0x82d00000 + n*0x4000 (TX), 0x82d02000 + n*0x4000 (RX), n=0..7; **9th instance 0x80170000 / 0x80172000 = WAN (PON) BBH** | Broad-Band Handlers: the DMA engines between the MACs (via XLIF) and SBPM/FPM |
| DSPTCHR | 0x82d20000 | Dispatcher/reorder: virtual ingress queues (VIQ) → Runner tasks |
| UBUS_MSTR | 0x82d28000, 0x82d29000 | XRDP UBUS masters (enable, hysteresis) |
| UBUS_SLV | 0x82d2a000 | XRDP UBUS slave decode (VPB/APB windows) and **RNR_INTR_CTRL ISR/ISM/IER/ITR (+0x80..0x8c)** |
| XLIF_* | 0x82d2b000 + n*0x200 | XLMAC interface per port (RX/TX enable, flow control, EEE) |
| SBPM | 0x82d2c000 | SRAM Buffer Pool Manager (4096 x 128 B buffers in PSRAM) |
| DMA | 0x82d2c800 (DMA0), 0x82d2cc00 (DMA1), 0x82d2d000 (SDMA0), 0x82d2d400 (SDMA1) | DDR DMA and SRAM DMA used by BBHs |
| TCAM, HASH, NATC*, CNPL, BAC_IF | 0x82e00000.., 0x82e20000, 0x82e50000.., 0x82e48000, 0x82e40000.. | classification/flow lookup, counters/policers, Runner accelerator interface. Needed for offload, not for CPU-only I/O |

---

## 2. The minimal datapath: Broadcom's U-Boot "reduced XRDP" (GPL)

### 2.1 Files [V]
`release/src-rt-5.04{axhnd.675x,behnd.4916}/bootloaders/u-boot-2019.07/drivers/net/bcmbca/` (copied to
`work/eth/ref/uboot_xrdp/`):
- `xrdp/data_path_6858.c` (2,399 lines, identical in both trees): `const access_log_tuple_t init_data[]`.
- `xrdp/access_logging.{c,h}`: the replay engine. Tuple word0 = `size[31:28] | op[27:24] | addr[23:0]`
  (addr is an offset from 0x82000000), word1 = value. Ops: 0 WRITE, 1 MWRITE (SRAM), 2 MEMSET and
  3 MEMSET_32 (value = length, the next tuple = {address, fill}), 4 SLEEP, 5 SET_CORE_ADDR, 6 STOP,
  7 MEMSET_ADAPTIVE_32, 8 WRITE_6BYTE_ADDR (absolute address), 9 POLL.
- `xrdp/bcmbca_xrdp_api.c`: `bcmbca_xrdp_init/send/recv`. `xrdp/rdp_drv_sbpm.c`: SBPM alloc/connect/free.
- `xrdp/rdd_data_structures.h`: 6858 constants (`RNR_REGS_ADDR_CORE0 0x82280000`, `SBPM_ADDRS 0x82d2c000`,
  `PSRAM_MEM_ADDRS 0x82600000`, `SBPM_MAX_BUFFER_NUMBER 0xFFF`, BB IDs, GEN45 descriptor macros).
- `xrdp_full/REDUCED_XRDP_README` and `gen_data_path_init_data.sh`: the log was produced by building
  U-Boot in "FULL" mode against the `XRDP_CFE2` project with access logging, booting it, and converting
  the console log.
- The Ethernet glue `bcmbca_xrdp_eth.c` is only shipped as `.o` (binary), but it is a thin U-Boot
  `eth_ops` wrapper around the API above plus LPORT/PHY calls; the 6858 LPORT driver is GPL in
  `5.04axhnd.675x/bootloaders/u-boot-2019.07/drivers/net/bcmbca/lport/` (`lport_drv.c`,
  `serdes_access.c`, `merlin_direct_access.c`, 6858 AG files).

`Makefile`: for 6858 the build defines `-DXRDP_SBPM -DGEN45` (packets live in SBPM/PSRAM, not DDR).

### 2.2 Decoded init sequence (6858) [V]
Decoder: `tools/xrdp_initlog_decode.py` → `work/eth/xrdp/uboot_6858_init.txt` (1,816 annotated lines;
every register write named, with non-zero fields decoded) and `uboot_6858_init_blockorder.txt`.

| # | Block (writes) | Register-level content |
|---|---|---|
| 1 | UBUS_SLV (4), UBUS_MSTR[0,1] (2), QM UBUS_TOP (6) | `UBUS_SLV.VPB_BASE=0x82d00000 / VPB_MASK=0xffe00000`, `APB_BASE=0x82e00000 / mask 0xffe00000`; `UBUS_MSTR[n].HYST_CTRL = 0x00020002` (CMD_SPACE=2, DATA_SPACE=2); QM slave windows `VPB 0x82140000`, `APB 0x82100000`, `DQM 0x82180000`, masks 0x000c0000 |
| 2 | Runner core 0 memories | `SET_CORE_ADDR[0]=0x82200000`; MEMSET RNR_MEM[0] 16 KiB = 0; MEMSET RNR_CNTXT[0] 2 KiB = 0; MEMSET_32 RNR_INST[0] 8 KiB = 0xfc000000 (NOP); **178 instruction words** written into RNR_INST[0] +0x000..+0x34f (the core-0 microcode; extracted to `work/eth/xrdp/uboot_6858_core0_fw.bin`, big-endian words) |
| 3 | RNR_REGS[0] | `CFG_SCH_CFG=4` (scheduler mode), `CFG_DDR_CFG=0xf0` (DMA_BASE), `CFG_PSRAM_CFG=0x826` (DMA_BASE = 0x82600000 >> 20) |
| 4 | Core 0 SRAM/contexts | 8 words at RNR_MEM[0]+0x60 (data structures); 384 words into RNR_CNTXT[0] (thread contexts: entry PCs and registers) |
| 5 | RNR_REGS[0..15] | `CFG_GLOBAL_CTRL = 0x01ffe700` on all 16 cores (MICRO_SEC_VAL=999, i.e. 1 µs = 1000 cycles [I: 1 GHz Runner clock]); EN stays 0 |
| 6 | RNR_QUAD[0] | `POWERSAVE_CONFIG=0xf20` (TIME_COUNTER=0x20, powersave on cores 0-3), `DMA_ARB_CFG=0x20` (CONGEST_THRESHOLD=2), `EXT_FLOWCTRL_CONFIG_TOKEN_VAL[20]=1`, `[16..19]=8` (PSRAM slave tokens) |
| 7 | BBH_RX[0..7] (17-18 each) | `SDMACFG=0x0e0e` (NUMOFCD=14, EXCLTH=14), `BBCFG=0x00301216` (SDMABBID=0x16, DISPBBID=0x12, SBPMBBID=0x30), `DISPVIQ` (NORMALVIQ = port, with 0↔4 swapped), `MINPKT0=0x40404040`, `MAXPKT0/1=0x05ee05ee` (1518), `SOPOFFSET=0x12` (18), `PERFLOWTH=0xff`, `FLOWCTRL` timer, `SBPMCFG` MAXREQ=15 |
| 8 | BBH_TX[0..7] (18 each) | `BBCFG_1_TX = 0x17301614` (DMASRC=0x14, SDMASRC=0x16, SBPMSRC=0x30, FPMSRC=0x17), `DMACFG_TX/SDMACFG_TX` (DESCSIZE=0x1c, MAXREQ), `DDRCFG_TX=0x20200003` (BUFSIZE=3, HNSIZE0/1=0x20), `RNRCFG_2[0]=0x00010034` (PTRADDR=0x34, TASK=1: the Runner thread and SRAM address the BBH reports TX progress to), `LAN_CONFIGURATIONS_PDBASE/PDSIZE` (PD FIFO 8+8), `PDEMPTY=1`, `EEE=1`, `TS=1`, `DFIFOCTRL` (BBH_TX[0]: PSRAMSIZE=0xbf DDRSIZE=0x17f PSRAMBASE=0x180, the XFI FIFO), `TXTHRESH` (BBH_TX[0]: DDR 0x15f, SRAM 0x9f) |
| 9 | SBPM (1) | `REGS_INIT_FREE_LIST = 0x03ffc000` (INIT_OFFSET=0xfff: 4096 buffers) |
| 10 | DMA[0..3] (17-26 each) | per peripheral: `CONFIG_NUM_OF_WRITES` (SDMA chunk count), `NUM_OF_READS`, `U_THRESH`, `PRI=0x84`, `PERIPH_SOURCE` (RX/TX BB ids, e.g. 0x2625 = BBH_RX/TX 3), `MAX_OTF` |
| 11 | DSPTCHR (1,358) | 1,023 x `BDRAM_DATA` (free linked list), 50+50 congestion thresholds, `QUEUE_MAPPING_CRDT_CFG[viq]` (BB_ID of the BBH RX, TRGT_ADD 2 = normal, 3 = exclusive), `MASK_DLY_Q`, `Q_INGRS_LIMITS`, `FLLDES_*`, `QDES_*`, pool sizes, load balancing (tasks → Runner group), `REORDER_CFG_VQ_EN` |
| 12 | XLIF_RX_IF[0..7], XLIF_TX_IF[0..7] | `IF_DIS=0`, `IF_ENABLE=0`, `TX_THRESHOLD=12` |
| 13 | Enables | `BBH_RX[n].GENERAL_CONFIGURATION_ENABLE=3` (PKTEN, SBPMEN) for n=0..7; `DSPTCHR.REORDER_CFG_DSPTCHR_REORDR_CFG` enable; `QM.GLOBAL_CFG_QM_ENABLE_CTRL=0x307` (FPM_PREFETCH, REORDER_CREDIT, DQM_POP, RMT_FIXED_ARB, DQM_PUSH_FIXED_ARB); `UBUS_MSTR[0,1].EN=1`; **`RNR_REGS[0].CFG_GLOBAL_CTRL=0x01ffe701` (core 0 runs)** |
| 14 | Core-0 SRAM bytes | 7 byte writes at RNR_MEM[0]+0x63e..0x656 (values 0x20,8,0x10,0x18,0,0x28,0x30: a per-port table, again with ports 0 and 4 swapped) |

Things this sequence does **not** do: no FPM init, no QM queue setup, no DDR packet buffers, no
interrupts, only Runner core 0. The Merlin trees have no 6858 `XRDP_CFE2` sources, so the
access-logged firmware cannot be rebuilt from source, but it is shipped as data inside a GPL-2.0+ file.

### 2.3 Packet I/O with this firmware [V, from `bcmbca_xrdp_api.c` / `rdd_data_structures.h`]
`core0 = 0x82200000` (RNR_MEM[0]).

**RX (polling):** the Runner places received packets in SBPM buffers (PSRAM, 128 B each, chained) and
writes a 16-byte descriptor into a 64-entry FIFO at **core0+0x800**. Each word is big-endian
(`swap4bytes`).
```c
/* GEN45 SBPM-mode RX descriptor in Runner SRAM (U-Boot CPU_RX_DESCRIPTOR), bit numbers of each
 * 32-bit word AFTER swap4bytes(). Same word layout as the DDR descriptor in §4.3. */
struct xrdp_sram_rx_pd {
	u32 w0;  /* [31:14] bn0 (first SBPM buffer, 18 bits); [13:0] reserved (fpm_idx in FPM mode)    */
	u32 w1;  /* [31:17] rsvd; [16] abs; [15:2] packet_length (14); [1] is_chksum_verified; [0] rsvd */
	u32 w2;  /* [31] is_src_lan; [30] rsvd; [29:25] source_port (5); [24:13] rsvd/wan_flow;          */
	         /* [12:6] data_offset (7); [5:0] reason (6)                                            */
	u32 w3;
};
```
Note [V, `bcmbca_xrdp_api.c` lines 27-66]: U-Boot declares this with C bit-fields, first-declared field
in the least significant bits: `w0 = {reserved0:14, bn0:18}`, `w1 = {reserved2:1, is_chksum_verified:1,
packet_length:14, abs:1, reserved1:15}`, `w2 = {reason:6, data_offset:7, reserved3:12, source_port:5,
reserved7:1, is_src_lan:1}`. (Pass 1 printed `w1`/`w2` of this struct with the bit order reversed; the
comments above are corrected.)
Procedure: invalidate the FIFO, read entry `idx`; `w1 == 0` means empty; copy the SBPM chain to a host
buffer (`drv_sbpm_copy_list`: walk with `SBPM.REGS_GET_NEXT` +0x24 / `GET_NEXT_RPLY` +0x28, data at
`0x82600000 + bn*128`, first buffer starts at `data_offset`); free it with
`SBPM.REGS_BN_FREE_WITHOUT_CONTXT` (+0x38 = head_bn | sa<<14 | ack<<31, poll +0x3c bit31); zero the
four words; `idx = (idx+4) % 256`. The 6858 build swaps `source_port` 0 and 4 in software
(`fix_bbh_rx_index`, "flowid swaps rx bbh").

**TX:** allocate SBPM buffers (`SBPM.REGS_BN_ALLOC` +0x4 = sa<<14, poll `BN_ALLOC_RPLY` +0x8:
valid bit0, bn [14:1], rdy bit31), copy the frame in (18 B headroom in the first buffer), chain with
`BN_CONNECT` (+0x1c) / `BN_CONNECT_RPLY` (+0x20). Then fill the single BBH TX descriptor at
**core0+0x50** (GEN45 layout below), write the BB destination to **core0+0xdc**, write the packet length
last, and ring the doorbell. The BB destination comes from the LPORT number through
`__rdd_cpu_tx_get_bb_id()` (`rdd_data_structures.h:198`): `tx_map[lport] = {40, 34, 36, 38, 32, 42, 44}`
(`BB_ID_TX_BBH_n` = 32 + 2n, BBH 7 = 52), so eth0 → 40 (BBH 4) and eth4 → 32 (BBH 0). Doorbell:
`writel(1, 0x82280004)` (`RNR_REGS[0].CFG_CPU_WAKEUP`, thread 1). Flow control: wait until the
descriptor length reads 0 and `ingress_count - egress_counter < 8`, where the per-port 8-byte egress
counters are at core0+0x180 + 8*port (ports 0 and 4 swapped).
```c
/* GEN45 BBH TX descriptor at core0+0x50, byte offsets, big-endian bit numbering inside each byte */
/* +0  bit7      SOF                                                         */
/* +4  bit7      LAST;  +4..7 u32 bits [20:14] BN_NUM (7 bits)               */
/* +6  u16 [13:0] PACKET_LENGTH  (write last)                               */
/* +8  bit6      TARGET_MEM_1;   +8..11 u32 [17:0] BN1_FIRST (last buffer)   */
/* +12 bit7 AGG_PD, bit6 TARGET_MEM_0, bit5 ABS; +12 u16 [12:2] SOP;          */
/* +12..15 u32 [17:0] BN0_FIRST (first buffer)                               */
```

### 2.4 What this means
- [V] Everything needed for a working (slow) Ethernet driver on 6858 is GPL: init values, the Runner
  microcode, buffer management and descriptor formats. No blob has to be extracted from `rdpa.ko`.
- [I] Throughput is limited by per-packet MMIO polling of SBPM (several register round trips per 128 B
  chunk), one TX descriptor in flight, and CPU copies. Expect tens to low hundreds of Mbit/s. Good
  enough for bring-up, management and a 1G LAN at low load; not for 2.5G/10G WAN routing.
- [U] The log was recorded on a Broadcom reference board. It enables all 8 BBHs and XLIFs; the LPORT
  side (XLMAC, QEGPHY power, Merlin SerDes) is separate and must be done first (doc 02 §2).

### 2.5 Minimal bring-up recipe: eth0 (LAN, 1G) + eth4 (WAN, 10GBASE-T) (added pass 2)
This is the shortest path to "CPU sends and receives on one LAN port and the WAN port". Each step is
tagged with its source.

| Step | What | Source / tag |
|---|---|---|
| 0 | **Power and reset XRDP.** `PowerOnDevice()` on PMB `XRDP` (bus 1 dev 12, 3 zones), `XRDP_QM` (13), `XRDP_RC_QUAD0..3` (14-17); then for each: `sr_control &= ~0xff`, then `|= 0xff` (reset pulse). Then `apply_ubus_credit_each_master()` for QM, DQM, NATC, DMA0/1, RQ0-3 and `ubus_master_rte_cfg()` | [V-source] GPL `pmc/impl1/pmc_xrdp.c::pmc_xrdp_init` (5.02axhnd.675x tree) and `pmc_drv.h`; [V] the stock kernel runs it as an early initcall (`Toggle reset of XRDP core...` right after `NET: Registered protocol family 16` on every boot). Mainline has no 6858 PMB table (doc 10 §3), so an open driver needs one or must poke the PMB directly |
| 1 | **Power LPORT** (`pmc_lport_init`, PMB bus 1 dev 9) and configure the LPORT mux: port 0 = XLMAC0 ↔ QEGPHY (GMII), port 4 = XLMAC1 ↔ Merlin16 core 0 lane 1 (XFI). QEGPHY power-up and base address, Merlin firmware and speed, BCM84891 firmware load over MDIO 0x1e | doc 02 §2-3, §9; GPL `lport_drv.c`, `serdes_access.c`, `phy_drv_ext3.c` |
| 2 | **Replay** `data_path_6858.c` unchanged (§2.2). No reserved memory needed: SBPM mode uses only PSRAM | [V] |
| 3 | **Fix the frame-size limits.** The log sets `BBH_RX[n].MAXPKT0/1 = 1518`, so VLAN-tagged full-size frames (1522 bytes with FCS) are probably dropped. That includes Bell's WAN: PPPoE at MTU 1492 inside VLAN 35 gives 1500-byte Ethernet payloads, i.e. 1522-byte tagged frames. Rewrite `MAXPKT0/1` of BBH_RX[4] (eth0) and BBH_RX[0] (eth4) to e.g. 1536 after the replay. TX is limited to `RDD_CPU_TX_MAX_BUF_SIZE` = 2048 | [V] log values and `bcmbca_xrdp_api.c`; [I] consequence; [U] whether the BBH counts the FCS. Stock firmware uses `bs /d bbh s pkt_size0 wan 48 1536` for the same purpose (doc 02 §4.4) |
| 4 | **Enable XLMAC TX/RX** on LPORT 0 and 4 and start PHY autoneg (phylink) | doc 02 |
| 5 | **RX:** poll the core-0 SRAM FIFO (§2.3); `source_port` after `fix_bbh_rx_index` is the LPORT (0 or 4); hand the frame to netdev eth0 or eth4 | [V] |
| 6 | **TX:** SBPM alloc + copy, BBH TX descriptor, `tx_map[lport]` (40 for eth0, 32 for eth4), doorbell thread 1 | [V] §2.3 |

Optional, [I]: the six unused BBH pairs and XLIFs can stay as the log configures them. Nothing transmits
to them, and an idle 1G LPORT port without a link carries no traffic. Pruning their writes from the
replay is a later clean-up, not a requirement.
eth4 at 2.5G/10G still goes through this single-core, single-descriptor path. Expect the WAN to be
CPU-copy-bound well below line rate (§2.4).

---

## 3. The full-featured template: XRDP_CFE (5.02L.07) and the stock `rdpa.ko`

### 3.1 Sources [V]
- `release/src-rt-5.02L.07p2axhnd/rdp/projects/XRDP_CFE/`: `BCM6858_B0_{sources,gpl_sources,fw_sources}.list`,
  `drivers/rdp_subsystem/BCM6858/data_path_init.{c,h}` (1,219 lines), `drivers/rdp_subsystem/xrdp/rdp_drv_{bbh_rx,bbh_tx,dma,dqm,fpm,qm,rnr,sbpm,dis_reor,cnpl,...}.c`,
  `drivers/rdp_subsystem/cpu/rdp_cpu_ring*.{c,h}`, `drivers/rdd/BCM6858/{rdd_init.c,rdd_data_structures_auto.{c,h},rdd_runner_defs_auto.h,rdd_map_auto.c}`,
  `firmware_bin/BCM6858/runner_fw_{0,1,2}.c` + `predict_runner_fw_{0,1,2}.c` (DUAL/GPL).
  The listed `drivers/rdp_subsystem/BCM6858/autogen/*` files are missing from the Merlin copy; the
  5.04 `XRDP_AG.h` (§1) replaces their header part.
- Stock `rdpa.ko` (decompiled with Ghidra: `work/eth/ghidra/dec/rdpa_logic.c`, all non-accessor
  functions; `bcm_enet.c`; scripts `work/eth/ghidra/scripts/Eth_DecompSel.java`).

### 3.2 Firmware images [V]
| Set | Images | Non-NOP words | Cores | Notes |
|---|---|---|---|---|
| U-Boot reduced (5.04) | 1 (in the access log) | 178 | core 0 | SBPM-only CPU datapath |
| XRDP_CFE (5.02L.07) | `runner_fw_0..2` (8,192 words each) | 385 / 617 / 342 | 3 (RNR_LAST=2) | image 0 = DS TM (egress to BBH TX), image 1 = CPU RX + WAN direct, image 2 = CPU TX |
| Stock `rdpa.ko` | `fw_binary_0..4` (32 KiB each) + `RUNNER_TABLES` (54,920 B) | 835 / 3,753 / 1,837 / 1,779 / 5,205 | 16, via `rdp_core_to_image_map` | production: flow lookup, TM, WLAN/DHD offload, multicast, speed test |

All three differ (no shared prefix). The XRDP_CFE and U-Boot images are GPL-labelled; the
`rdpa.ko` images are proprietary (extract locally if ever needed). Copies of the `rdpa.ko` images:
`work/eth/xrdp/rdpa_ko_fw_binary_*.bin`.

### 3.3 Init order [V: XRDP_CFE `_data_path_init()`; same order in `rdpa.ko` `FUN_0018b740` called from `data_path_init`]
1. **UBUS bridge** (`ubus_bridge_init`): UBUS_SLV VPB/APB windows, UBUS_MSTR hysteresis (§2.2 row 1).
   Stock only, before that: `drv_rnr_quad_ubus_decode_wnd_cfg(0, 0, board_size_log2, BIU, coherent)`
   (decode window 0 = all DDR, coherent) and window 1 = the FPM pool (`rdp1`) non-coherent
   (`CONFIG_BCM_FPM_COHERENCY_EXCLUDE`: "CCI-400 can't handle 10Gig bandwidth").
2. **BBH profiles** (`bbh_profiles_init`, software only): picks a buffer split by XFI port
   (`p_dpi_cfg->xfi_port`: emac0, emac4 or none; here emac4). SDMA/DMA chunks per BBH from
   `g_bbh_buff_num`. Stock `rdpa.ko` .data: `g_dma_to_bbh_x = {DMA0: 3,5,6,7,8(PON),-; DMA1: 4,1,2,0}` and
   `g_bbh_buff_num` profile xfi4 = `{5,5,5,5,12},{14,6,6,6}`: the XFI BBH gets 14 of 32 DMA1 slots.
3. **FPM** (`fpm_init`): `drv_fpm_init(rdp_ddr_pkt_base, fpm_buf_size)`, `POOL1_XON_XOFF_CFG` (4096/4160),
   `FPM.INIT_MEM=1` and poll until clear, `POOL1_EN=1`, interrupt status/mask.
4. **QM** (`qm_init` → `drv_qm_init`): FPM base in 256 B units, buffer size, 4 FPM user groups
   (DS, US, WLAN, all-pass; the log shows `FPM group configuration set: DS = 20740, US = 40260`), WRED
   profile for CPU RX, copy-decision profiles, then **DQM** (`dqm_init`: `DQM.FPM_ADDR` =
   phys(FPM.POOL1_ALLOC_DEALLOC), `BUF_BASE`, `BUF_SIZE`), then every queue: US range, DS range, drop
   queue, CPU RX queue, CPU RX copy queues; then `QM.RUNNER_GRP_*` = Runner groups (group 0 = DS TM core,
   pd_fifo/update_fifo addresses in core SRAM; group 3 = CPU RX task; group 8 = CPU RX copy).
5. **CNPL** counters/policers memory init.
6. **Runner** (`runner_init`): `drv_rnr_cores_addr_init`, `drv_rnr_mem_init` (zero SRAM/context),
   `drv_rnr_load_microcode` (fw_binary_N into RNR_INST of each core per `rdp_core_to_image_map`),
   `drv_rnr_load_prediction`, `RNR_REGS.CFG_SCH_CFG` (DS TM core: `DRV_RNR_4SP_12RR`), `drv_rnr_dma_cfg`
   (`CFG_DDR_CFG.DMA_BASE = (hi<<12)|(lo>>20)` of the FPM pool, buffer size; `CFG_PSRAM_CFG` =
   PSRAM_MEM >> 20, 128 B), `rdd_data_structures_init` (all firmware tables in core SRAM), `RNR_FREQ`,
   parser (`RNR_QUAD` TCP flags, profile, exception bits, PPP protocol codes, AH detection), quad
   power-save, DMA arbitration, flow-control tokens.
7. **BBH RX** (`bbh_rx_cfg` for each LAN BBH): SDMA chunks, dispatcher and SBPM BB ids, normal/exclusive
   VIQ, min/max packet size (64 / MTU), SOP offset 18, per-flow thresholds.
8. **BBH TX** (`bbh_tx_init`): MAC type EMAC, Runner source = DS TM core, `tcont_addr` / `ptr_addr` /
   `task_number` (egress-counter table and LAN thread in the DS TM image), PD FIFO base/size (8+8),
   FPM and SBPM source ids, DMA/SDMA/DDR profile, FPM base, `EEE=1`, max OTF reads, data FIFO and TX
   thresholds (XFI-sized for one BBH), `TS=1`.
9. **SBPM** (`sbpm_init`): `REGS_INIT_FREE_LIST(base, 0xfff)`, user-group 0 thresholds (`excl_low_thr=900`).
10. **DMA/SDMA** (`dma_sdma_init`): per DMA engine `MAX_OTF`, then per peripheral (BBH) `NUM_OF_WRITES`
    (SDMA), `NUM_OF_READS`, `U_THRESH`, `PRI`, `WEIGHT`, `PERIPH_SOURCE`.
11. **Dispatcher/reorder** (`dispatcher_reorder_init`): reset credits; one normal and one exclusive VIQ
    per BBH (VIQ 0..8 normal, 9..17 exclusive, PON exclusive gets guaranteed buffers); VIQ 18 = CPU TX
    egress (to the CPU TX image, reorder), VIQ 19 = CPU RX copy; Runner group 1 = "WAN direct" thread of
    image 1; pool limits; `drv_dis_reor_queues_init`, `tasks_to_rg_init`, `free_linked_list_init`,
    `drv_dis_reor_cfg`, `LOAD_BALANCING_LB_CFG(1,2)`.
12. **Enable** (`rdp_block_enable`): wait for `SBPM.REGS_INIT_FREE_LIST` RDY; XLIF RX `IF_DIS=0`,
    TX `IF_ENABLE=0`, `URUN_PORT_ENABLE=0`, `TX_THRESHOLD=12` per port; `BBH_RX.ENABLE(pkt,sbpm)=1`;
    dispatcher enable; `QM_ENABLE_CTRL` (5 bits); `UBUS_MSTR.EN=1` x2; (stock: `TCAM.OP`); `RNR_ENABLE=1`
    on each core.

The U-Boot log (§2.2) is a recording of a reduced form of this sequence (FPM/QM parts are skipped
there because the XRDP_CFE2 build is `XRDP_SBPM`).

### 3.4 Memory
- **[V] `rdp1` = 64 MiB at 0x18c00000**: the FPM DDR packet-buffer pool (`rdp_ddr_pkt_base`, named
  `FPMPOOL_BASE_ADDR_STR` in `BcmMemReserveGetByName`). Mapped (no `no-map` for 6858 in
  `kernel/dts/bcm_rsvdmem_64.dtsi`) and excluded from coherency via UBUS decode window 1.
  [I] With the 61,000 tokens of the DS+US user groups this implies 1 KiB tokens (64 Ki tokens).
- **[V] `rdp2` = 8 MiB at 0x1cc00000**: `no-map`; Runner DDR tables (`RDP_PARAM2`, Broadcom calls it
  flow memory: NATC/flow-cache context tables). [I] Not needed for CPU-only I/O.
- **[V] Host buffers** come from BPM (`BPM: num of buffers=53544, buf size=3008`), not from `rdp1`.
- `dhd0..2` (3 x 11 MiB) are WiFi offload rings, not needed.

### 3.5 BBH numbering quirk (eth4 vs BBH 0) [V]
`rdpa.ko` .data `rdpa_emac_to_bb_id_tx = {40, 34, 36, 38, 32, 42, 44, 52}` for emac0..7. With the 6858 BB
ids (`BB_ID_TX_BBH_n = 32 + 2n`, U-Boot `rdd_data_structures.h`), **emac0 (LPORT 0, eth0) uses BBH
instance 4 and emac4 (LPORT 4, eth4, the XFI port) uses BBH instance 0**; emac1-3 and 5-7 map 1:1.
The GPL code mirrors this everywhere: `normal_viq = 4 - bbh_id` for BBH 0/4, egress counter and DS TM
thread index `4 - bbh_id`, U-Boot `fix_bbh_rx_index()`. BBH instance 0 has the long XFI data FIFO
(`BBH_TX[0].DFIFOCTRL` = 0xbf/0x17f/0x180 in the log, `XFI_EMAC_PORT 0`). An open driver must carry an
explicit `lport → bbh` table: `{0:4, 1:1, 2:2, 3:3, 4:0, 5:5, 6:6, 7:7}`.

---

## 4. How `bcm_enet` talks to the Runner (stock Linux)

### 4.1 Objects and calls [V: `bcm_enet.ko` imports + decompile; GPL impl7 `runner.c`, `rdp_ring.c`]
- Ports: `create_rdpa_port()` → `bdmf_new_and_set(rdpa_port_drv(), …)` with `index = rdpa_if_lan0+n`
  (or `wan0` for the GbE WAN EMAC), `cfg` (emac, sal/dal, ls_fc), egress TM object (`rdpa_egress_tm`,
  SP queues), `cpu_obj` link.
- CPU queues: `enetxapi_queues_init()` sets `rx_q[0] = 4` (high) and `rx_q[1] = 3` (low), 1024 entries
  each (`FUN_001052c8(cpu, chan, i, 0x400)` → `rdpa_cpu_rxq_cfg_set` with `rx_isr`, interrupt coalescing).
  Trap reasons 5, 6, 0x0b, 0x20, 0x21, 0x22, 0x25 → queue 4, everything else → queue 3
  (`rdpa_system_cpu_reason_to_tc` / `rdpa_cpu_tc_to_rxq`). A dynamic meter is attached to queue 3.
- RX: NAPI thread `bcmsw_rx` → `rdpa_cpu_packet_get(rdpa_cpu_host, q, &info)` → FKB built in the buffer
  headroom (`enetxapi_rx_pkt`), recycle hook `bdmf_sysb_recycle` (buffer goes back to the feed ring).
  Interrupts: `rdpa_cpu_int_enable/disable/clear`, `rdpa_cpu_queue_is_full` for NAPI reschedule.
- TX: `port_runner_dispatch_pkt_lan()` → `rdpa_cpu_send_sysb(sysb, &info)` with
  `info.method = rdpa_cpu_tx_port (0)`, `info.port = rdpa_if`, `info.cpu_port = host`,
  `info.x.lan.queue_id`. (`rdpa_cpu_send_raw` exists and is used by `bcmgpon` for PLOAM/OMCI.)

### 4.2 CPU ring map on the HH4000 (kernel.log, every boot) [V]
| Ring (queue) | Entries x size | Created by | Role |
|---|---|---|---|
| 16 | 8192 x 8 B | `rdpa` system init (`cpu_rxq_feed_and_recycle_rings_init`) | **feed ring**: empty host (BPM) buffers for the Runner to fill (`rdd_ring_init` type 2) |
| 17, 18 | 16384 x 8 B | same | **recycle rings**: TX-completed host buffers returned to the CPU; 17 on the image-1 core, 18 on the image-2 core (type 1) |
| 19, 20 | 2048 x 16 B | same | **CPU TX rings** (type 3): 19 = `rdpa_cpu_tx_port`/egress, 20 = `rdpa_cpu_tx_bridge`/ingress (from `rdpa_cpu_send_sysb`: `local_50 = 0x13` vs `0x14`) |
| 3, 4 | 1024 x 16 B | `bcm_enet` | Ethernet RX low / high priority |
| 7, 8 / 10, 11 / 13, 14 | 1024 x 16 B | `wfd` (wl0 / wl1 / wl2, `qmask 0x3`) | WiFi forwarding RX (traffic from Runner to the WLAN radios) |
| 9 / 12 / 15 | 128 x 16 B | `dhd` per radio (`dhd_runner_cfg_cpu_queue`) [I from order and strings] | DHD CPU queue |
| 0-2, 5, 6 | unused on this unit | | |

`rdd_ring_init()` rejects ring ids > 20 (`0x14 < param_1`), so 21 rings exist.
The DHD offload doorbells are logged directly: `TX wakeup info: reg = <0x82282004>, val = <0xa..0xc>`,
`RX wakeup info: reg = <0x82282004>, val = <0xd..0xf>`, i.e. `RNR_REGS[2].CFG_CPU_WAKEUP` with thread numbers.

### 4.3 Descriptor and ring-table layouts
All Runner-visible structures are **big-endian**; the stock driver byte-swaps every 32-bit word
(`swap4bytes`, `swap4bytes64`).

**CPU RX descriptor, 16 B, in DDR** [V: GPL `rdp_cpu_ring_defs.h` for BCM6858 in
`5.04axhnd.675x/rdp/drivers/rdp_subsystem/BCM6858/` (copy in `work/eth/ref/xrdp_ag/linux6858/`), and
`rdpa_cpu_helper.h` 5.02L.07]:
```c
/* big-endian view (as defined under !_BYTE_ORDER_LITTLE_ENDIAN_), MSB first */
struct cpu_rx_desc {
	u32 host_buffer_data_ptr_low;           /* w0: buffer phys [31:0] */
	u32 host_buffer_data_ptr_hi:8,          /* w1[31:24]: phys [39:32]                     */
	    reserved0:7,
	    abs:1,                              /* w1[16]: 1 = host (absolute) buffer          */
	    packet_length:14,                   /* w1[15:2]                                    */
	    is_chksum_verified:1,
	    reserved2:1;
	u32 is_src_lan:1, color:1,              /* w2[31], w2[30]                              */
	    source_port:5,                      /* w2[29:25] RDD vport (rdd_vport_to_rdpa_if_map) */
	    wan_flow_id:12,                     /* w2[24:13] (or ssid/vport for WLAN)          */
	    data_offset:7,                      /* w2[12:6]                                    */
	    reason:6;                           /* w2[5:0] rdpa_cpu_reason                     */
	u32 word3;                              /* is_exception, is_rx_offload, is_ucast, mcast_tx_prio, wl metadata */
};
```
**Ring handshake (corrected in pass 2) [V: `rdpa.ko` `rdp_cpu_ring_read_packet_refill` @0x91d78 and
`rdp_cpu_ring_get_packet`, Ghidra `rdpa_logic.c`]:** the stock RX rings have **no per-descriptor ownership
bit**. Pass 1 described a `rdpa_cpu_rx_pd_get()` ownership check, but no such function exists in this
`rdpa.ko` (`nm` has no `rx_pd_get` or `rest_desc` symbols). The handshake works on indices instead:
- The Runner advances `write_idx` (BE u16 at +6 of the ring's `RDD_CPU_RING_DESCRIPTOR` in core SRAM, §4.3
  table below). The host keeps a cached read index and a cached write index per ring. When they are
  equal it re-reads `write_idx` from SRAM, and if they are still equal the ring is empty.
- After it has consumed `n` entries, the host writes `read_idx = (read_idx + n) & (number_of_entries*32 - 1)`
  (BE u16 at +0xc) into **every core** whose image has `CPU_RING_DESCRIPTORS_TABLE` (the loop skips table
  addresses of 0xffffff). So entry counts must be powers of two and multiples of 32.
- Decode per entry: buffer = `w0 | w1[31:24]<<32` (phys), converted with `phys - memstart_addr + PAGE_OFFSET`;
  `packet_length = w1[15:2]`; `is_chksum_verified = w1[1]`; `data_offset = w2[12:6]`;
  `color = w2[30]`; `source_port = w2[29:25]`; `wan_flow_id = w2[24:13]` when `is_src_lan = 0`; for WLAN
  vports (9..16) `ssid = w2[24:21]`.
- The `refill` variant then kicks the feed-ring refill (`rdpa_feed_ring_refill_kick`) every
  `refill_every` packets, so the Runner always has empty BPM buffers on ring 16.
`abs` (w1[16]) marks an absolute host address and is not an ownership flag.

**Feed descriptor (ring 16) and recycle descriptor (rings 17, 18), 8 B** [V same file]:
```c
struct cpu_feed_desc    { u32 ptr_low; u32 ptr_hi:8, abs:1, reserved:23; };                 /* BE */
struct cpu_recycle_desc { u32 ptr_low; u32 ptr_hi:8, abs:1, from_feed_ring:1, reserved:22; };
```

**CPU TX descriptor, 16 B (`RDD_RING_CPU_TX_DESCRIPTOR`, rings 19/20)** [V: `rdpa.ko`
`dump_RDD_RING_CPU_TX_DESCRIPTOR` field strings and shifts, `rdpa_cpu_send_sysb` encoder]:
| Word | Bits | Field |
|---|---|---|
| w0 | 31 | is_egress |
| w0 | 30:22 | first_level_q (egress QM queue, 9 bits) |
| w0 | 21:8 | packet_length (14) |
| w0 | 7:0 | sk_buf_ptr_high (sysb pointer [39:32]) |
| w1 | 31:0 | sk_buf_ptr_low_or_data_1588 (sysb pointer, returned on the recycle ring) |
| w2 | 31 / 30 / 29 / 28 | color / do_not_recycle / flag_1588 / lan |
| w2 | 27:20 | wan_flow_source_port_union (LAN vport or WAN flow) |
| w2 | 19 / 18 / 17 / 16 | fpm_fallback / sbpm_copy / target_mem_0 / abs |
| w2 | 15:14 | ingress_or_egress_0 |
| w2 | 13:10 | ssid |
| w2 | 9 | egress_dont_drop |
| w2 | 7:0 | pkt_buf_ptr_high (packet data phys [39:32]) |
| w3 | 31:0 | pkt_buf_ptr_low_or_fpm_bn0 |

TX procedure [V, `rdpa_cpu_send_sysb`]: pad to 60 bytes; look up the egress queue for (port, queue_id)
in the TM hash; build the descriptor with `abs=1` and the physical data and sysb addresses
(`virt + memstart_addr + 0x4000000000` = linear-map phys); check free space using the Runner's read
index from `RDD_CPU_TX_RING_INDICES_VALUES_TABLE` (in core SRAM; ring 19 at +0, ring 20 at +4); write
the 16 bytes at `ring_base + wr*16`; `dmb`; publish the new write index (u16 BE) at
`INDICES_TABLE + 4*ring + 2` in every core that hosts the table; then wake the CPU TX thread.
On completion the Runner pushes the sysb pointer to recycle ring 17/18 (unless `do_not_recycle`), and
`_rdpa_recycle_thread_handler` frees it.

**Runner-side ring descriptor (`RDD_CPU_RING_DESCRIPTOR`, 16 B, in core SRAM)** [V: XRDP_CFE
`rdd_data_structures_auto.h` + `rdpa.ko` `rdd_ring_init`]:
| Byte | Field |
|---|---|
| 0 [7:3] | size_of_entry (bytes: 8 or 16) |
| 0[2:0]..1 | number_of_entries (11 bits; the stock driver writes `(entries + 31) >> 5`, feed ring `>> 6`) |
| 2-3 | interrupt_id (u16: bit index in `UBUS_SLV.RNR_INTR_CTRL`) |
| 4-5 | drop_counter |
| 6-7 | write_idx (Runner) |
| 8-11 | base_addr_low |
| 12-13 | read_idx (host updates) |
| 14 | low_priority_threshold |
| 15 | base_addr_high (phys [39:32]) |

`rdd_ring_init(ring_id, type, base, entries, entry_size, irq, …)` writes this into
`CPU_RING_DESCRIPTORS_TABLE + 16*ring_id` (type 0, RX), `CPU_RECYCLE_RING_DESCRIPTOR_TABLE` (type 1, also
sets the recycle index DDR address and coalescing defaults, and wakes thread 3 on the image-2 core),
`CPU_FEED_RING_DESCRIPTOR_TABLE` (type 2, plus refill-interrupt threshold 0x20 and reservations), or
`CPU_TX_RING_DESCRIPTOR_TABLE` (type 3), in every core whose image has that table.

### 4.4 Interrupts [V]
- `rdpa_cpu_int_enable(port, q)`: reads `UBUS_SLV.RNR_INTR_CTRL_IER` (0x82d2a088), sets bit
  `irq_index & 31`, writes it back (spinlocked). ISR status is `RNR_INTR_CTRL_ISR` (+0x80), `ISM` (+0x84,
  masked status), `ITR` (+0x8c, test).
- **ISR is write-1-to-clear** [V/I]: `rdpa_cpu_int_clear()` calls `ag_drv_ubus_slv_rnr_intr_ctrl_isr_set(1 << bit)`.
  It writes only the single bit, with no read-modify-write. That is correct only if the register is W1C.
- **Interrupt bit = RDD ring id** [V]: `cpu_attr_rxq_cfg_write` calls `rdd_ring_init(ring, type, base,
  entries, entry_size, irq = ring, ...)` with the same `cpu_rdd_rxq_idx_get()` value for both, and
  `rdpa_cpu_int_clear`/`rdpa_cpu_packet_get` use the same per-queue byte (cpu object +0xb94+q) as the bit
  and as the ring. So bcm_enet's queues 3 and 4 raise `RNR_INTR_CTRL` bits 3 and 4, i.e. **GIC SPI 131 and 132**
  [I for the SPI numbers, from the table below].
- GIC lines: `6858_intr.h`: `INTERRUPT_XRDP_QUEUE_0..31 = SPI_TABLE_OFFSET(32) + 128..159`, i.e.
  **GIC SPI 128-159**, one per `RNR_INTR_CTRL` bit; `INTERRUPT_XRDP_FPM` SPI 161, `QM` 163,
  `DSPTCHR` 164, `SBPM` 165, `LPORT_INTR_0/1` SPI 28/26. The stock DTB has no XRDP interrupt nodes; the
  numbers are hard-coded in the Broadcom kernel. (The `SPI + 32` convention matches the PCIe nodes in
  `kernel/dts/6858/96858.dts`.)
- Per-core firmware interrupts: `RNR_REGS[n].CFG_INT_CTRL/CFG_INT_MASK` (INT0..9, FIT_FAIL).

### 4.5 What an open driver must reimplement
| Piece | Stock implementation | Open replacement |
|---|---|---|
| XRDP init | `rdpa.ko` `data_path_init` | replay of §2.2, later a C port of XRDP_CFE `data_path_init.c` using the GPL header |
| Firmware load | `drv_rnr_load_microcode/prediction`, `rdd_data_structures_init` | load the GPL U-Boot or XRDP_CFE images (`request_firmware`), write RNR_INST/PRED/CNTXT/MEM, start cores |
| Buffer pools | FPM (DDR) + SBPM, BPM host buffers | SBPM only (phase 1); FPM pool in a reserved 64 MiB region (phase 2) |
| CPU RX | rings + feed/recycle threads | phase 1: poll the SRAM PD FIFO; phase 2: DDR rings + page_pool + NAPI |
| CPU TX | CPU TX rings 19/20 + recycle | phase 1: SBPM copy + BBH descriptor; phase 2: XRDP_CFE CPU TX image |
| Port objects, TM, reasons | bdmf objects (`rdpa_port`, `egress_tm`, `cpu` reasons, meters) | not needed for a dumb NIC: every RX packet goes to the CPU, every TX names its port |
| MAC/PHY | kernel `lport_drv`, `phy_drv_*` | phylink MAC/PCS + PHY drivers (doc 02 §9) |

---

## 5. PON WAN vs Ethernet WAN in the Runner (notes for later)

[V] From `rdpa.ko` (`data_path_init_gbe`, `data_path_init_fiber`, `FUN_00189d90`, `rdpa_cpu_send_sysb`),
XRDP_CFE, and the RU tables:
- **Ethernet WAN** (`RdpaWanType=GBE`, `WanOEMac=EMAC4`): the WAN is one of the 8 LAN-type BBHs
  (BBH instance 0 for emac4). `data_path_init_gbe(emac)` only re-points the upstream TM:
  `rdd_ag_us_tm_wan_0_bb_destination_table_set(rdpa_emac_to_bb_id_tx[emac])`, US BBH TX FIFO size 8,
  FIFO bytes threshold 0x1000, `set_fpm_budget`. QM US queues feed one WAN channel.
- **PON WAN** uses the **9th BBH pair at 0x80170000 (TX) / 0x80172000 (RX)** (the `WANBBH` physical
  region in doc 02 §2.1), attached to the GPON/XGPON/EPON MAC instead of an XLIF.
  `data_path_init_fiber`: `rdd_mac_type_cfg` (GPON/XGPON/EPON), `BBH_RX.MAC_MODE`, `PLOAM_EN`,
  `USER_PRIORITY3_EN`, `PATTERN_RECOG/PATTERN_EN`, then enables the WAN BBH RX and the PON
  **exclusive VIQ** in `DSPTCHR.REORDER_CFG_VQ_EN` (VIQ 8 normal / 9 exclusive per doc 14 §2.3, with guaranteed
  dispatcher buffers; PLOAM and OMCI use the exclusive path), `rdd_ghost_reporting_mac_type_init` and
  `rdd_ghost_reporting_timer_set` (Runner-generated DBA status reports to the MAC), `rdd_tm_epon_cfg`
  for EPON, then the same US TM BB-destination/FIFO setup.
- **T-CONTs and GEM ports:** upstream QM queues are grouped per T-CONT channel. `FUN_00189d90` configures
  the US queue range (`drv_qm_get_us_start/end`, separate EPON range), the Runner group for the US TM
  core, and `QM.EPON_OVERHEAD_CTRL`. `rdpa_cpu_send_sysb` maps GEM flow → T-CONT channel with
  `rdpa_gem_flow_id_to_tcont_channel_id` (GPON/XGPON) or `rdpa_llid_queue_id_to_channel_id` (EPON);
  up to 40 US channels (`< 0x28` in the hash lookups). The PON BBH TX holds per-queue-pair PD FIFOs
  (XRDP_CFE `g_wan_queue_to_rnr[TX_QEUEU_PAIRS=20]`).
- **Downstream:** the PON BBH RX "flow" is the GEM port index (256 flows with per-flow packet-size
  selectors, `BBH_RX` flows 0-31 / groups); the CPU RX descriptor carries it as the 12-bit `wan_flow_id`.
- **The mode is latched once** (added pass 2) [V]: `rdpa_mw.ko` `wan_scratchpad_get` reads `RdpaWanType`/`WanOEMac`
  at insmod. `GBE` with `EMACn` selects `data_path_init_gbe(n)`. A malformed `WanOEMac` falls back to GBE
  EMAC4 ("Safe recovery mode"). `XGS`/`XGPON1`/`NGPON2`/`GPON`/`EPON` select the fiber path. All 9 logged
  boots ran GBE/EMAC4. The factory scratchpad copy says XGS (doc 03 §2.2, doc 02 §5.1). In XGS mode eth4
  stays an ordinary LAN BBH (instance 0) and the WAN BBH pair at 0x8017xxxx carries the XGS-PON MAC.
  An open Ethernet-only driver can ignore the setting and simply treat LPORT 4 as the WAN netdev.
- [I] Consequence: an open PON stack needs, on top of the Ethernet datapath, the WAN BBH pair, the US
  TM firmware image with per-T-CONT scheduling and ghost/DBA reporting, and GEM flow tables. The GPL
  firmware sets (U-Boot, XRDP_CFE) have no PON US path; this is the strongest reason PON stays
  hybrid-only (doc 08 §6).

---

## 6. Recommended code structure (mainline style) [I]

```
drivers/net/ethernet/broadcom/xrdp/
  xrdp_regs.h          generated from XRDP_AG.h + rdpa_fields.json (FIELD_PREP/GENMASK style), bases from DT
  xrdp_core.c          platform driver "brcm,bcm6858-xrdp": ioremap, clocks/resets [U], reserved-memory,
                       block init (ubus, fpm, qm/dqm, bbh rx/tx, sbpm, dma, dispatcher, enable)
  xrdp_runner.c        core reset/start, request_firmware() of GPL images, RNR_INST/PRED/CNTXT/MEM load
  xrdp_sbpm.c          SBPM alloc/connect/free/copy (phase 1)
  xrdp_ring.c          DDR RX/feed/recycle/TX rings, ring-table writes in core SRAM (phase 2)
  xrdp_irq.c           RNR_INTR_CTRL irq_chip demux (32 lines) or per-queue IRQs from DT
  xrdp_netdev.c        one netdev per LPORT port, NAPI, ndo_start_xmit, lport→BBH table {4,1,2,3,0,5,6,7}
drivers/net/ethernet/broadcom/bcm6858_lport.c   XLMAC + LPORT mux + LED, phylink MAC
drivers/net/pcs/pcs-bcm-merlin16.c              Merlin16 SerDes (XFI / 2500BASE-X / SGMII)
drivers/net/mdio/mdio-bcm6858-lport.c           LPORT MDIO (C22/C45)
drivers/net/phy/bcm84881.c (+ firmware loader)  BCM84891 XFI host mode
firmware: brcm/xrdp-6858-uboot-core0.bin (from data_path_6858.c), later brcm/xrdp-6858-cfe-{0,1,2}.bin
```
DT sketch: `xrdp@82000000 { reg = <0x82000000 0xe51000>, <0x80170000 0x4000>; interrupts = <GIC_SPI 128..>;
power-domains = <&pmb BCM6858_PMB_XRDP>, <&pmb ..._XRDP_QM>, <&pmb ..._RC_QUAD0..3>;  /* needs a new brcm,bcm6858-pmb table */
memory-region = <&rdp1>; brcm,lport = <&lport>; }` (doc 02 §7). `memory-region` is only needed from
phase 2 (FPM/DDR rings).

Design notes:
- Keep the access-log replay as a stepping stone only; convert it into named register writes with the
  decoder output (`uboot_6858_init.txt`) so it can be reviewed and tuned (MTU, port count).
- The Runner is big-endian on its SRAM structures; use `__be32` types and `be32_to_cpu`.
- Treat XRDP as a "switch with CPU port": the netdevs are LPORT ports, the Runner is a shared DMA
  engine. Bridging in software (or later `switchdev`) on top.

---

## 7. Effort estimate [I]
| Step | Content | Effort (experienced kernel developer) |
|---|---|---|
| 0 | LPORT MDIO, QEGPHY power-up, XLMAC/LPORT for one 1G port (doc 02 §9) | 2-4 weeks |
| 1 | XRDP replay init + U-Boot SBPM RX/TX (polling, NAPI timer) on eth0 | 1-2 weeks |
| 2 | All 4 LAN ports + eth4 at 1G/2.5G (Merlin16 XFI/2500BASE-X + BCM84891 firmware loader) | 4-8 weeks |
| 3 | Interrupt-driven DDR rings with the XRDP_CFE 3-image firmware (FPM pool, feed/recycle, CPU TX ring), port of `data_path_init.c` | 6-10 weeks |
| 4 | Throughput work (GRO, coalescing, multiple RX queues, cache handling) | open-ended |
| 5 | Any hardware offload (flow tables, NATC, TM) with production firmware | months; would need the undocumented RDD table layouts of `fw_binary_*` and is proprietary |
| 6 | PON datapath (WAN BBH, US TM with T-CONTs, DBA ghost reporting) | months, no GPL firmware for it |
| Σ 0-2 | First usable homelab router: 4x 1G LAN + eth4 WAN at 1G/2.5G, polled SBPM path, plus PMB power-domain support for XRDP/LPORT | **about 3-5 months** (added pass 2) |

The biggest schedule risks are the Merlin16 SerDes port (step 2: about 9k lines of GPL code, and
hardware is needed to debug it) and the BCM84891 XFI host mode. The mainline driver does not even
match this PHY's ID (doc 10 §3). The XRDP part is comparatively well specified.

---

## 8. Open questions
1. **Answered (pass 2)** [V-source/V-log]: XRDP power and reset are done by `pmc_xrdp_init()` (PMB XRDP,
   XRDP_QM, RC_QUAD0-3, reset toggle, UBUS credits). The stock kernel runs it as an early initcall. See §2.5
   step 0. Still open [U]: whether the RDPPLL/UNIPLL channel settings are left at the boot ROM/U-Boot
   defaults (the `pll_ch_freq_set` calls are `#if 0` in the GPL source).
2. [U] Whether the U-Boot log's core-0 firmware also handles eth4 at 2.5G/10G line rate (it has one TX
   descriptor slot and a 64-entry RX FIFO).
3. **Mostly answered (pass 2)** [V/I]: ISR is W1C (single-bit write in `rdpa_cpu_int_clear`) and
   `interrupt_id` = RDD ring id (§4.4). Still open [U]: hardware confirmation, and whether one GIC SPI per
   bit really fires as `6858_intr.h` says (no XRDP interrupt nodes in the stock DTB).
4. **Partly answered (pass 2)** [V]: for RX and recycle rings the field holds `entries/32`, and the host
   reads it back as an index mask `N*32-1`. The feed ring is written as `(entries+31)>>6` (8192 → 128). Open
   [U]: how the Runner firmware interprets the feed value. Copy the stock value.
5. **Answered (pass 2)** [V]: there is no ownership bit. RX uses write/read indices in the SRAM ring
   descriptor (§4.3).
6. [U] Runner clock (MICRO_SEC_VAL=999 suggests 1 GHz) and `RNR_FREQ` value used by the stock kernel.
7. [U] (new) BBH RX `MAXPKT` counting (with or without FCS), which decides the safe value for step 3 of §2.5.

## 9. Artifacts (this pass)
| Path | Content |
|---|---|
| `tools/ag_fields_extract.py` | accessor field recovery + merge + GPL cross-check (pass 2: `_mismatch.txt` now lists all 16 differing registers, including those whose accessor fields overlap GPL fields; previously only 2) |
| `tools/xrdp_initlog_decode.py` | decoder for U-Boot `data_path_<chip>.c` access logs |
| `work/regdb/rdpa_fields.{h,json}`, `rdpa_fields_mismatch.txt` | merged 6858 XRDP register/field DB |
| `work/eth/ref/xrdp_ag/` | GPL `XRDP_AG.h` for 6858, 63158, 6856, 6846, 4912; `linux6858/rdp_cpu_ring_defs.h` |
| `work/eth/ref/uboot_xrdp/` | U-Boot 2019.07 bcmbca xrdp/xrdp_full/lport-adjacent sources (5.04 trees) |
| `work/eth/xrdp/uboot_6858_init.txt`, `_blockorder.txt` | decoded 6858 init log |
| `work/eth/xrdp/uboot_6858_core0_fw.bin` | core-0 microcode from the log, RNR_INST +0x000..+0x34f = 212 words (big-endian): the 178 words the log writes, with the 34 gaps left at the NOP fill 0xfc000000 |
| `work/eth/xrdp/rdpa_ko_fw_binary_{0..4}.bin` | stock firmware images (proprietary, local use only) |
| `work/eth/ghidra/dec/{rdpa_logic,bcm_enet,rdpa_dump}.c` | Ghidra 12.2 decompiles (`Eth_DecompSel.java`) |

## 10. Revision note (pass 2, 2026-11-04)
Pass 1 was interrupted after writing this file. Pass 2 re-checked it against the scratch artefacts and `rdpa.ko` decompiles and changed the following:
- §1.2: the mismatch file listed only 2 of the 16 differing registers. `tools/ag_fields_extract.py` was patched and re-run (statistics unchanged) and now lists all 16. Corrected the claim that none of them is on the init path: `BBH_RX.MINPKT0` and `DSPTCHR.Q_INGRS_LIMITS` are, and the GPL split is the correct one for both.
- §2.3: fixed the bit order of `w1`/`w2` in the SRAM RX descriptor comment and added the `tx_map` LPORT→BB-id table.
- §2.5 (new): concrete eth0 + eth4 bring-up recipe, including XRDP PMB power and reset (`pmc_xrdp_init`) and the 1518-byte `MAXPKT` limit in the U-Boot log.
- §4.3: removed the non-existent `rdpa_cpu_rx_pd_get` ownership check. RX uses write/read indices in the SRAM ring descriptor, verified from `rdp_cpu_ring_read_packet_refill`.
- §4.4: ISR clear is a single-bit write (W1C), and the interrupt bit equals the RDD ring id.
- §5: how the WAN mode is latched (`rdpa_mw` `wan_scratchpad_get`), and what GBE vs XGS means for the BBHs.
- §6, §7, §8: PMB power domains in the DT sketch, a total effort row, and open questions 1, 3, 4 and 5 answered or narrowed.
- §9: the core-0 image is 212 words, of which the log writes 178.
