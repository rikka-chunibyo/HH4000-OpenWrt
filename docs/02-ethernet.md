# 02 - Ethernet subsystem (Bell Home Hub 4000 / Sagemcom F@ST 5689 / BCM6858)

Scope: the four 1G copper ports, the 10G copper port (eth4), the BCM6858 LPORT block (XLMAC + QEGPHY + Merlin SerDes), MDIO, the external 10G PHY, the Runner (XRDP) datapath and the Broadcom driver stack (bcm_enet impl7, rdpa, bdmf, pktflow/pktrunner), how the WAN is selected, and what an OpenWrt port would need.

Paths below are relative to `/path/to/HH4000-OpenWRT` (`$P`). Scratch work and artifacts are in `$P/work/eth/`.

Labels used throughout:
- **[V] Verified**: backed by a cited file, symbol, offset, string or log line.
- **[I] Inferred**: reasoned from evidence or from Broadcom reference sources of the same SDK version, but not directly observed on this unit.
- **[U] Unverified**: open question.

---

## 0. TL;DR

| netdev | Rear-panel jack (§2.4) | LPORT port / Runner BBH | MAC | Host interface | PHY | MDIO addr / clause | Notes |
|---|---|---|---|---|---|---|---|
| eth0 | LAN 1 [I] | 0 / BBH 4 | XLMAC0 port 0 | GMII (internal) | BCM6858 QEGPHY port 0 | 0x01, C22 | 1G LAN. LEDs on GPIO62/63 |
| eth1 | LAN 2 [I] | 1 / BBH 1 | XLMAC0 port 1 | GMII (internal) | QEGPHY port 1 | 0x02, C22 | 1G LAN. LEDs on GPIO65/66 |
| eth2 | LAN 3 [I] | 2 / BBH 2 | XLMAC0 port 2 | GMII (internal) | QEGPHY port 2 | 0x03, C22 | 1G LAN. LEDs on GPIO68/69 |
| eth3 | LAN 4 [I] | 3 / BBH 3 | XLMAC0 port 3 | GMII (internal) | QEGPHY port 3 | 0x04, C22 | 1G LAN. LEDs on GPIO71/72 |
| eth4 | separate silver "10G" jack [V] | 4 / BBH 0 | XLMAC1 port 4 | **XFI** (10GBASE-R) on LPORT Merlin16 core 0 lane 1 | **external BCM84891 B0** ("Blackfin", ID 0x35905091), firmware loaded over MDIO at every boot; reset on **GPIO80** (active-low) | **0x1e, C45** | 10GBASE-T multi-gig. Used as the Ethernet WAN in the logs (links seen at 1G and 2.5G). LEDs driven by the PHY itself |

- **[V]** The datapath is the XRDP Runner. `bcm_enet.ko` sends with `rdpa_cpu_send_sysb` and receives from Runner CPU rings (`rdpa_cpu_packet_get`). It cannot pass packets without `rdpa.ko` (proprietary Runner firmware and drivers) and `bdmf.ko`.
- **[V]** The MAC/PHY layer (LPORT, XLMAC, Merlin SerDes, QEGPHY, BCM84891 firmware loader, MDIO) is built into the stock kernel, and its source is in Broadcom's dual-licence GPL `shared/opensource/drv/{lport,phys}`. The BCM84891 firmware embedded in the stock kernel is byte-identical to the GPL `blackfin_b0_firmware.h` (SHA-256 `63bb2324…c8fa`).
- **[V]** A public SDK tree with the same version (ASUS-Merlin `release/src-rt-5.02L.07p2axhnd`) includes the **XRDP_CFE BCM6858_B0 project**. It is a minimal Runner datapath (CPU<->port only) with Runner firmware arrays, all under the dual GPL label. *(Pass 2:)* The piece that was missing here, the BCM6858 XRDP register/field header, has since been found in the 5.04 Merlin tree (`XRDP_AG.h`). It matches `rdpa.ko`. The 5.04 trees also contain a GPL U-Boot "reduced XRDP" CPU datapath for 6858, which is a recorded init sequence plus SBPM RX/TX code. The Runner spec is now in [11-xrdp-runner.md](11-xrdp-runner.md).
- Feasibility verdict: see section 9. In short, the quickest working setup is OpenWrt userspace on the stock 4.1.52 kernel plus the stock modules. A modern kernel needs new drivers: an MDIO driver (easy), LPORT/SerDes (port the GPL code), a BCM84891 XFI and firmware path (port), and a minimal XRDP datapath (hard, but GPL reference code exists).

---

## 1. Board identification and board parameters

### 1.1 Board id
- **[V]** NVRAM (`raw/backup/HH4000-backup/mtd0.bin`, offset `0x10684`): `szBoardId = "FAST5689"`. At `0x1069c`: number of MAC addresses `0x0a` (10). At `0x106a0`: base MAC `<device-mac>`.
  ```
  00010680: 0000 0000 4641 5354 3536 3839 0000 0000  ....FAST5689....
  00010690: 4731 00ff 0000 0000 8000 0000 0a00 0000  G1..............
  000106a0: <device-mac> ff00 ffff ffff ffff ffff  4]..Dn..........
  ```
- **[V]** Bootargs (`raw/backup/HH4000-backup/bootargs.txt`) contain `board_type=00030103`, a Sagemcom value that the Broadcom boardparms code does not use.
- **[V]** The kernel log prints `VOICE: bsp board found ... SagemcomFast5689_Bell`.
- **[V]** MAC usage: LAN ports and the bridge use the base MAC `<device-mac>`. The WAN VLAN devices use base+1 (`eth4.35 MAC address set to <device-mac>`, kernel.log.1), which matches `MACAddress` for PHY6 in `conf/backup-cfg-Ethernet-16.xml`.

### 1.2 Kernel symbols
- **[V]** The stock kernel has kallsyms: 43,136 symbols, text only (`CONFIG_KALLSYMS_ALL` is not set). I converted it with `vmlinux-to-elf`:
  - `$P/work/eth/vmlinux.elf`: ELF with symbols, base `0xffffffc000080000`, ready for Ghidra or IDA.
  - `$P/work/eth/syms.txt`: `nm -n` output.
- **[V]** The kernel config is embedded (`CONFIG_IKCONFIG`). I extracted it to `$P/work/eth/kernel.config`. Relevant lines:
  `CONFIG_BCM96858=y, CONFIG_BCM_ENET=m, CONFIG_BCM_ENET_IMPL=7, CONFIG_BCM_XRDP=y, CONFIG_BCM_PON=y, CONFIG_BCM_PON_XRDP=y, CONFIG_BCM_RDPA=m, CONFIG_BCM_RDPA_GPL=m, CONFIG_BCM_RDPA_MW=m, CONFIG_BCM_PKTFLOW=m, CONFIG_BCM_PKTRUNNER=m, CONFIG_BCM_BPM=y, CONFIG_BCM_ETHWAN=y, CONFIG_BCM_FTTDP=y, # CONFIG_BCM_FTTDP_G9991 is not set, # CONFIG_BCM_EXT_SWITCH is not set, CONFIG_BCM_PON_WAN_TYPE_AUTO_DETECT=y, CONFIG_BCM_WANTYPEDET=m, CONFIG_BCM_SFP=m, # CONFIG_PHYLIB is not set, # CONFIG_MODVERSIONS is not set, # CONFIG_NF_TABLES is not set, # CONFIG_NAMESPACES is not set, CONFIG_PREEMPT=y, CONFIG_COMPAT=y, CONFIG_ARM64_VA_BITS=39`.

### 1.3 The boardparms table for FAST5689
The boardparms tables are built into the kernel (`bp_parse_mac_dev`, `bp_parse_phy_dev`, `BpGetEthernetMacInfoArrayPtr`, …). The table for this board is at `vmlinux.bin` file offset **0x8102f8** (VA `0xffffffc0008902f8`). It is an array of `bp_elem_t {u32 id; u32 pad; u64 value}`. Raw dump: `$P/work/eth/bp_FAST5689_raw.txt`.

**Note on enum ids.** Sagemcom's `bp_defs.h` is not the stock 5.02L.07 one. The enum is shifted by +2 from `bp_usGpioLedBlStop` onward, and Sagemcom added its own ids at the end, for example `BpGetXfiResetGpio = 0x153`, `BpGetXfiPowerEnGpio = 0x154`, `BpGetPhyRstAllGpio = 0x15f`, `BpGetSwitchRstGpio = 0x160`. I established the ids in two ways:
- (a) I disassembled every `BpGet*()` accessor to extract the id constant it passes to `BpGetUs/Ul/...`. Output: `$P/work/eth/bp_ids_from_code.txt`, script `scripts/bpids.py`.
- (b) I aligned the stock `968580XREF` table compiled into the same kernel (offset `0x815188`) with its GPL source (`boardparms_6858.c`, ASUS-Merlin 5.02L.07 tree).

Both methods agree for every Ethernet id: `0x22=bp_ucPhyType0, 0x24=bp_ucPhyAddress, 0x25=bp_usConfigType, 0x26=bp_ulPortMap (also used by BpGetNumGePorts), 0x27..0x2b=bp_ulPhyId0..4, 0x92=bp_ulPortFlags, 0x133=bp_usNetLed0, 0x134=bp_usNetLed1, 0x137=bp_ulNetLedLink, 0x138=bp_ulNetLedActivity, 0xbb=bp_ulInterfaceEnable, 0x163=bp_last`.

Decoded Ethernet section of `FAST5689` **[V]**. Bit definitions are from `boardparms.h` 5.02L.07: `MAC_IFACE_S=23`, `PHY_EXTERNAL=1<<19`, `PHY_INTEGRATED_VALID=1<<20`.

```
0x8105b8  0x22 bp_ucPhyType0        = 1            BP_ENET_INTERNAL_PHY
0x8105c8  0x24 bp_ucPhyAddress      = 0
0x8105d8  0x25 bp_usConfigType      = 6            BP_ENET_CONFIG_MMAP
0x8105e8  0x26 bp_ulPortMap         = 0x1f         lport 0..4
0x8105f8  0x2b bp_ulPhyId4          = 0x0698001e   = 0x1e | MAC_IF_XFI(13<<23) | PHY_INTEGRATED_VALID | PHY_EXTERNAL
0x810608  0x27 bp_ulPhyId0          = 0x01800001   = 0x01 | MAC_IF_GMII(3<<23)
0x810618       bp_usNetLed0         = 0x803e  (GPIO62, active low)
               bp_ulNetLedLink      = 0x4     (BP_NET_LED_SPEED_1G)
               bp_ulNetLedActivity  = 0x4
               bp_usNetLed1         = 0x803f  (GPIO63, active low)
               bp_ulNetLedLink      = 0x3     (10M|100M)
               bp_ulNetLedActivity  = 0x3
0x810678  0x28 bp_ulPhyId1          = 0x01800002   (LEDs GPIO65 = 1G, GPIO66 = 10/100)
0x8106e8  0x29 bp_ulPhyId2          = 0x01800003   (LEDs GPIO68 / GPIO69)
0x810758  0x2a bp_ulPhyId3          = 0x01800004   (LEDs GPIO71 / GPIO72)
0x8108e8  0xbb bp_ulInterfaceEnable = 0x06000000 NAND, 0x02000000 PCM, 0x09000000 HS_SPI, 0x0d000000 LPORT
0x810948  0x163 bp_last
```
- **[V]** This table has no `bp_ulPortFlags` and no NetLed entries for port 4.
- **[V]** The sibling table `FAST5689V0` (offset `0x810958`, probably an earlier hardware revision) is identical for Ethernet except that it has `0x92 bp_ulPortFlags = 0x20` (PORT_FLAG_SWAP_PAIR) on port 4. The NVRAM selects `FAST5689`.
- **[V]** Other board items that matter to Ethernet: `0xfc bp_usGpioWanSignalDetected = GPIO49` and `0x12c bp_usGpioOpticalModuleTxPwrDown = GPIO55`, both on the SFP/PON side. Sagemcom has XFI reset and power-enable GPIO accessors (`BpGetXfiResetGpio`, `BpGetXfiPowerEnGpio`), but **FAST5689 does not use their ids (0x153/0x154)**, so the 84891 reset is not described in boardparms. *(Pass 2, resolved:)* the reset is in Sagemcom's own BSP table instead. See the next item.
- **[V] PHY reset and enable GPIOs (pass 2).** The Sagemcom BSP table (`bsp_gpio_lkm.ko` `board_info_5689_bell`, decoded in `work/periph/bsp_bell_gpio.txt`, doc 04 §4.1) has **GPIO80 = id 0x0ba "Phy rst all", output, active-low, init-off** (reset deasserted). The stock U-Boot DT has the same pin as a hog: bank `gpio-controller@0xff800508` `p80 { gpios = <0x10 GPIO_ACTIVE_LOW>; output-low; line-name = "phy_reset"; }`, i.e. logically deasserted. It also hogs **GPIO4 `phy_enable` output-high** (not in the BSP table). `bsp_gpio_lkm` has a helper `bsp_reset_gpio_lan()`, reached from `bsp_notify_evt()`. For each of up to three GPIOs in the board's LAN-reset list it asserts the line, waits 15 µs (`__const_udelay(0xfba9)`), releases it and waits 100 µs (`__const_udelay(0x68dbc)`).
  - **[I, strong]** GPIO80 resets the BCM84891, the only external PHY. The QEGPHY is on-die and is reset through LPORT/PMC. GPIO4 is probably the 84891 (or its regulator) enable. Sagemcom's "reset 10G port" (`write45 4 0x1 0x0 0x8000`, §2.2) is an MDIO soft reset, not this GPIO.
  - **[U]** Whether GPIO80 is in the `bsp_reset_gpio_lan` list and whether anything triggers it in a normal boot. No BCM84891 datasheet timing is available here. The 15 µs pulse looks short, so an open driver should use conservative timing (for example 10 ms asserted, then 100 ms before the first MDIO access) **[I]**.
- **[V]** The 968580XREF_PHY reference board in the GPL `boardparms_6858.c` uses the same encoding: `{bp_ulPhyId4, .u.ul = 0x1e | MAC_IF_XFI | PHY_EXTERNAL | PHY_INTEGRATED_VALID}`. The HH4000 follows Broadcom's "XREF with external 10GBASE-T PHY at address 30" reference design.

---

## 2. Hardware blocks and port map

### 2.1 LPORT (BCM6858 "Lport" Ethernet complex)
Base addresses **[V]**. They are in the kernel's physical-base table at `vmlinux.bin` offset `0x81e2e0`, as `{id,size,phys}` records: EPON `0x80140000/0x4000`, WAN `0x80144000/0x1000`, GPON `0x80148000/0x12000`, NGPON2 `0x80160000/0xe000`, WANBBH `0x80170000/0x4000`, XRDP `0x82000000/0xe51000`, LPORT `0x80138000/0x6000`. These match the GPL `6858_map_part.h`.

| Sub-block | Address (GPL AG files `shared/opensource/drv/lport/ag/BCM6858_A0*_AG.c`) |
|---|---|
| XLMAC (per port, indirect) | 0x80138000, 0x80138400, 0x80138800, 0x80138c00 (XLMAC0 ports 0-3), 0x8013a000… (XLMAC1) |
| LPORT_MIB | 0x80139000… / 0x8013b000… |
| LPORT_CTRL | 0x8013c000 |
| LPORT_SRDS (Merlin control) | 0x8013c00c |
| LPORT_LED (per port) | 0x8013c074 + n*0xc |
| LPORT_RGMII | 0x8013c100 + n*0xc |
| XLMAC_CONF | 0x8013d000, 0x8013d200 |
| MIB_CONF | 0x8013d100, 0x8013d300 |
| **LPORT_MDIO** | **0x8013d400** (CMD at +0, CFG at +4). **[V]** The literal is present once in the kernel data at file offset 0x821ba8 |
| LPORT_INTR | 0x8013d500 |
| LPORT_MAB | 0x8013d700, 0x8013d800 |

LPORT mux rules from GPL `lport_drv.c` **[V-source]**:
- XLMAC0 owns ports 0-3. They can go to the QGPHY or to Merlin (≤2.5G). Port 0 alone can be 10G Merlin.
- XLMAC1 owns ports 4-7. Port 4 can be 10G Merlin. Ports 5-7 can be RGMII or Merlin.
- `validate_lport_configuration()` allows XFI only on ports 0 and 4. Ports sharing a Merlin lane: (0,7), (1,4), (2,5), (3,6).
- `serdes_access.c`: `port_to_core[4] = MERLIN_ID_0` and `port_to_lane[4] = MERLIN_LANE_1`, so **eth4 runs on LPORT Merlin16 core 0, lane 1**. This SerDes is not the PON/WAN SerDes, which `serdesctrl` drives through `/proc/serdes/wan/*`.

Kernel init trace **[V]** (each boot in `kernel.log*`). These are the `lport_init_driver` stages:
```
INTR!  MDIO!  QEGPHY!  XLMAC!  MUX!  SERDES!  MSBUS!  RGMII!  LED!
```

### 2.2 PHYs
- **[V] QEGPHY.** Internal quad GPHY, MDIO addresses 1-4, `PHY_TYPE_6858_EGPHY` (`bp_parse_phy_type()` in GPL `phy_bp_parsing_6858.c`: GMII goes to `PHY_TYPE_6858_EGPHY`, bus `BUS_TYPE_6858_LPORT`). `lport_qgphy_base_addr_set` is in the kernel. Log lines: `eth0 EGPHY:GMII:0x1 - Link Up 1000 Mbps Full duplex`, and likewise `eth1 ...0x2`, `eth2 ...0x3`, `eth3 ...0x4`.
- **[V] 10G PHY: Broadcom BCM84891, rev B0** ("Blackfin" family: BCM8489x/BCM5499x). Boot log:
  ```
  Detecting PHYs...
  84891    B0 3590:5091 --> 0x1e
  Loading firmware into detected PHYs...
  Firmware version: Blackfin B0 v02-02-06
  Loading firmware into PHYs: map=0x40000000 count=1        (bit 30 = MDIO addr 0x1e)
  Halt the PHYs processors operation
  Upload the firmware into the on-chip memory   0% … 100%
  Reset the processors to start execution of the code in the on-chip memory
  Verify that the processors are running: OK
  Verify that the firmware has been loaded with good CRC: OK
  Firmware loading completed successfully
  ```
  - PHY ID `0x3590:0x5091` matches `{ 0x3590, 0x5091, "84891    B0", &blackfin_b0 }` in the GPL `phy_drv_ext3.c`. *(Pass 2, from doc 10 §3:)* the same GPL table lists `0x3590:0x5081` as "84891L B0". Mainline `bcm84881.c` matches `PHY_ID_MATCH_MODEL(0x35905080)` under the name "BCM84891", which is really the 84891**L** ID, so **this board's PHY does not bind to the mainline driver** **[V]**.
  - Driver type is `PHY_TYPE_EXT3` (HSGMII/XFI with `PHY_EXTERNAL`). Link message: `eth4 EXT3:XFI:0x1e - Link Up 2.5 Gbps Full duplex` / `... 1000 Mbps ...`.
  - **The firmware is pushed over MDIO at every boot** (9 boots logged, 9 uploads), so the PHY does not boot from its own SPI flash. **[I]** There is probably no PHY flash on the board.
  - Firmware source **[V]**: the image is built into the kernel. The descriptor string `Blackfin B0 v02-02-06` is at file offset 0x8b9f98. The 294,720-byte image is at `vmlinux.bin` 0x872058, which I extracted to `$P/work/eth/bcm84891_blackfin_b0_v02-02-06.bin` (sha256 `63bb232495b57203af5e7f774657660955f3fec8519bd4edfba2d90f27d9c8fa`). It is **byte-identical** to `blackfin_b0_firmware[]` in the GPL tree (`shared/opensource/drv/phys/blackfin_b0_firmware.h`, label `DUAL/GPL`). An "A0 v00-02-04-2018-05-10" image is also linked in at 0x901e60. There are no PHY firmware files in the rootfs.
  - Init sequence (GPL `phy_drv_ext3.c::_phy_init`) **[V-source]**: PMA reset `1.0000=0x8000`, wait for `1.0000==0x2040`, line-side mode, `CMD_SET_XFI_2P5G_5G_MODE` (default `XFI_MODE_BASE_X` for both 2.5G and 5G unless `inter_phy_types` says otherwise), force auto-MDIX, wirespeed, pair swap, XFI polarity 0, caps PAUSE|REPEATER, LED type = firmware-controlled, then speed 10000 for the XFI mii type. MACsec support exists (`phy_macsec_*` symbols) and is not used.
  - **[I]** For 1G and 2.5G copper links the MAC side runs in 1000BASE-X / 2500BASE-X rather than 10G idle-stuffing. This follows from the `XFI_MODE_BASE_X` default and from `lport_serdes_speed_set` / `_get_serdes_speed` in `phy_drv_lport_serdes.c`, which report 10G/2.5G/1G from the LPORT rate. I have not confirmed it on hardware; check with `/proc/driver/lport` status on a live box.
- **[V]** Sagemcom userspace (`usr/lib/libhal-network.so.1.0.0`) controls the 10G PHY by writing to `/proc/driver/phy/cmd`:
  - `write45 4 0x1 0x0 0x8000` with the log message "reset 10G port". The first argument is the **phy_devices[] index (4)**, not the MDIO address (`phy_proc_cmd_write45()` in GPL `phy_drv.c`).
  - LED setup on 84891 vendor registers `1.A82C/A82E/A82F/A831/A83B/A8EC/A8EF/A8F0` (`HALPHY_PowerPhyLeds`). `A83B=0x8400/0x8410` turns LEDs on and `0x8001` turns them off. The same writes appear in kernel.log at every WAN link-up.
  - **[I]** These are the 848xx LED mask/control registers, so the 10G port LEDs come from the PHY's LED pins, not from SoC GPIOs. This is consistent with the board table having no NetLed entries for port 4.

### 2.3 LEDs
- **[V]** 1G ports: two LEDs each, driven by the LPORT LED block on SoC GPIO pins, all active low. LED0 = 1G link/activity, LED1 = 10/100 link/activity. Pins: GPIO 62/63 (eth0), 65/66 (eth1), 68/69 (eth2), 71/72 (eth3). The kernel has `lport_leds_init`. Doc 04 §4.1: pinmux function 6 = LED-controller outputs OPTLED 0/1, 3/4, 6/7, 9/10.
- **[V]** 10G port: LEDs on the BCM84891 (see above).
- Per-jack mapping (pass 2), assuming the eth→jack order of §2.4 **[I]**:

  | Jack | netdev | LED0 = 1G link/act | LED1 = 10/100 link/act |
  |---|---|---|---|
  | LAN 1 | eth0 | GPIO62 | GPIO63 |
  | LAN 2 | eth1 | GPIO65 | GPIO66 |
  | LAN 3 | eth2 | GPIO68 | GPIO69 |
  | LAN 4 | eth3 | GPIO71 | GPIO72 |
  | 10G | eth4 | 84891 LED pins (`1.A82C..A8F0`, firmware-controlled) | |

- **[U]** Which colour and which side of each RJ45 jack. The FCC photos (doc 10 §2) are too low-resolution to show this; it needs a look at the hardware. Apart from the jack LEDs, the board GPIO table has only the power LED (GPIO76-78 white/red/blue) and the OLED-button LEDs (doc 04 §4.1). There is no separate per-port LED on the front panel **[V doc 04 table]**.

### 2.4 Front-panel mapping
- **[V]** Data model (`conf/backup-cfg-Ethernet-16.xml`):
  - `PHY1..PHY4` = PhyNum 1..4, Role LAN.
  - `PHY5` = PhyNum 5, Role LAN, disabled.
  - `PHY6` = PhyNum 5, Role WAN, enabled, MAC `<device-mac>`.
  - PhyNum 5 is the 10G port, which can be either LAN or WAN.
- **[V] (pass 2) HAL tables** in `usr/lib/libhal-network.so.1.0.0` `.data`. `sBoardIntfDesc[i]` (0x18-byte entries) has port index `i` for i = 0..5. `sHostIntfDesc[i]` (0x1c-byte entries) is `{"eth<i>", type 6, 1, port mask 1<<i}` for eth0..eth4, plus `eth5` with type 4 and mask 0x20. `HALPRIV_getBoardInterfaceMatchingHost()` matches a host ifname to the board entry with the same port bit. `HALPRIV_getMaxEthernetPort()` and `getMaxMii()` both return 6. So **PhyNum n = board port n-1 = eth(n-1)**: PHY1..PHY4 = eth0..eth3 and PhyNum 5 (PHY5/PHY6) = eth4. Dump script: `work/eth/scripts/intfdesc.py`.
- **[V] (doc 10 §2, FCC photo `hh-004.jpg`)** The ports are on the **rear** panel. From left to right: PWR barrel, TEL 1, TEL 2, **LAN 1 .. LAN 4** (yellow, two ganged pairs), then a **separate silver RJ45 labelled "...10G"** with its own magnetics.
- **[I]** LAN *n* silkscreen = PHY*n* = eth(*n*-1). This rests on Sagemcom naming the data-model objects after the silkscreen, which is very likely but **[U]** not physically checked: plug one cable and watch `eth? Link Up`.

### 2.5 Crossbar and WAN SerDes
- **[V]** The kernel has the crossbar code (`crossbar_*`, `phy_drv_crossbar_*`), and `bcm_enet` imports it. **FAST5689 defines no crossbar entries**, so eth4 is fixed to lport 4 and the SFP/PON cage uses the separate WAN SerDes (`WAN_PHYS_BASE 0x80144000`, `/proc/serdes/wan`, owned by the PON agent).
- **[V]** The optical config (`conf/backup-cfg-Optical-8.xml`) whitelists some SFPs of type `ETHERNET` (Cisco-Avago ABCU-5710RZ copper SFP, Alcatel G010SP, Huawei MA5671A). Stock firmware can therefore also run the SFP cage as **Active Ethernet** through the EPON MAC (`WanOEMac=EPONMAC` in wanconf, see section 5). That path belongs to the PON agent. It is a second Ethernet-WAN option besides eth4.

---

## 3. MDIO

- **[V]** One LPORT MDIO master. Kernel functions: `lport_mdio22_rd/wr`, `lport_mdio45_rd/wr`, `lport_mdio_bus_init`, `bus_6858_lport_c22_read/write`, `bus_6858_lport_c45_read/write`. `rdpa.ko` also imports `lport_mdio22_*` and `lport_mdio45_*`.
- Register layout (GPL `BCM6858_A0_LPORT_AG.h`) **[V-source]**:
  - `LPORT_MDIO_CMD` at `0x8013d400`: `START_BUSY[29]`, `FAIL[28]`, `OP_CODE[27:26]`, `PHY_PRT_ADDR[25:21]`, `REG_DEV_ADDR[20:16]`, `DATA_ADDR[15:0]`.
  - `LPORT_MDIO_CFG` at `0x8013d404`: `FREE_RUN_CLK_ENABLE[13]`, `SUPRESS_PREAMBLE[12]`, `MDIO_CLK_DIVIDER[11:4]`, `MDIO_CLAUSE[0]` (clause 22 or 45 is a global mode bit; `lport_mdio.c` switches it per transaction).
- Devices on the bus **[V]**: 0x01-0x04 QEGPHY (C22) and 0x1e BCM84891 (C45, devices 1/3/7/30/31).
- Userspace access on stock firmware **[V]**:
  - `echo "read45 <phyidx> <dev> <reg>" > /proc/driver/phy/cmd` and `write45 <phyidx> <dev> <reg> <val>`. Also available: `read`, `write`, `read32`, `write32`, `status`, `list`, `crossbars`, `macsecenable*`. `phyidx` is the internal phy_devices[] index; Sagemcom uses 4 for the 84891.
  - `ethctl phy int|ext|extsw|ext32 <phy_addr> <reg> [<value>]`. Registers 0x1,0000-0x1f,ffff mean C45 `(dev<<16)|reg`. Also `ethctl <if> media-type`, `ethctl phy-map`, cable diagnostics, EEE.
  - `/proc/driver/lport/serdes_cmd` (`serdesctrl lport txcfg …`) for the Merlin TX equaliser.
- **[V] (pass 2, doc 10 §3)** There is no mainline driver or DT node for this MDIO block (no LPORT code in `drivers/net`, no MDIO node in `bcm6858.dtsi`). `mdio-bcm-unimac` is a different block. A new one would be about 150 lines (poll START_BUSY, set the CLAUSE bit).

---

## 4. Driver architecture and datapath

### 4.1 Layering
```
 Linux netdev eth0..eth4 (+ "bcmsw" root dev, ioctl SIOCETHSWCTLOPS etc.)
   bcm_enet.ko   (impl7, GPL, enet/impl7: enet.c port.c runner.c rdp_ring.c ...)
     |-- MAC/PHY: kernel built-in phy_drv/mac_drv  -> mac_drv_lport -> lport_drv (XLMAC/Merlin)
     |                                               -> phy_drv_6858_egphy / phy_drv_ext3 (84891)
     |-- datapath: rdpa_cpu_send_sysb() TX, rdpa_cpu_packet_get()/CPU rings RX, rdpa_port objects
   rdpa_gpl.ko  (GPL stubs/API: f_rdpa_* function pointers, bdmf type wrappers)
   rdpa.ko      (PROPRIETARY: XRDP drivers, ag_drv_* register layer, data_path_init_*, rdd, Runner fw_binary_0..4)
   bdmf.ko      (GPL: Broadcom Device Management Framework object model + shell)
   bcm_pondrv.ko(proprietary; hard dependency of rdpa.ko even for Ethernet WAN)
   rdpa_mw.ko   (GPL: middleware; reads scratchpad, creates rdpa "system" object with WAN type)
   rdpa_usr/rdpa_cmd (userspace ioctl bridges for bdmf_shell / rdpactl)
   pktflow.ko (fcache, proprietary) + pktrunner.ko (proprietary) + cmdlist.ko  -> HW flow offload
   bcmvlan.ko (vlanctl), bcm_ingqos, bcmmcast, wfd (Wi-Fi forwarding), bcm_spdsvc, bcmlibs
```
Evidence **[V]**:
- `modinfo` (all vermagic `4.1.52-5.02L.07 SMP preempt mod_unload aarch64`):

  | module | depends | license |
  |---|---|---|
  | bcm_enet | bdmf, rdpa_gpl | GPL |
  | rdpa | bdmf, rdpa_gpl, bcmlibs, bcm_pondrv | Proprietary |
  | rdpa_mw | bdmf, rdpa_gpl, rdpa | GPL |
  | pktrunner | rdpa_gpl, rdpa_mw, bdmf, bcmlibs, pktflow | Proprietary |
  | pktflow, bcmvlan, bcmlibs, cmdlist, bcm_ingqos | (none) | Proprietary |
  | bdmf, rdpa_gpl, rdpa_gpl_ext, rdpa_usr, rdpa_cmd, wfd | (see modinfo) | GPL |
  | sgcnet | (none) | "SAGEMCOM" (IGMP-stats helper, not needed for Ethernet) |

- `nm -u bcm_enet.ko` includes `rdpa_cpu_send_sysb, rdpa_cpu_packet_get, rdpa_cpu_int_enable/disable/clear, rdpa_cpu_queue_is_full, rdpa_port_drv, rdpa_port_get, rdpa_egress_tm_drv, rdpa_system_get, bdmf_new_and_set, bdmf_link`. It also imports **Broadcom kernel patches**: `blog_finit_args, nbuff_flushfree, fkb_flush, skb_headerinit, skb_header_alloc, bcm_sw_gso_xmit, netdev_path_set_hw_subport_mcast_idx`, as well as `phy_dev_*, mac_dev_*, crossbar_*, BpGetEthernetMacInfoArrayPtr, bp_parse_*`.
- `bcm_enet.ko` defines `port_runner_*`, `runner_get_pkt_from_ring`, `runner_ring_create_delete`, `enetxapi_queues_init`, `create_rdpa_port`, `_demux_id_runner_port`. Its CPU path is entirely Runner CPU rings.
- `rdpa.ko` is not stripped: 5,168 symbols, including 1,366 `ag_drv_*` register accessors, `data_path_init`, `data_path_init_gbe`, `data_path_init_fiber`, `data_path_init_basic`, `rdd_init`, `bbh_id_to_rdpa_emac`, `rdpa_emac_to_bbh_id_e`, and the **Runner firmware images `fw_binary_0..4`**.

**Conclusion [V]: Linux cannot send or receive on any port without the Runner.** The LPORT XLMACs connect only to the XRDP BBH (Broad-Band Handler). Even the CFE/U-Boot Ethernet path on 6858 has to load Runner firmware (see section 8.3). `bcm_enet` has no DMA engine of its own on this chip (impl7 `Makefile`: for 6858 it builds `runner.o runner_standalone.o rdp_ring.o`, not `enet_ring.o`/sysport).

### 4.2 Runner and buffer resources seen at boot [V]
- Reserved memory (stock DTB and log): `dt_reserved_rdp1` 64 MiB at `0x18c00000` (FPM packet buffer pool) and `dt_reserved_rdp2` 8 MiB at `0x1cc00000` (Runner DDR tables). There are also `dhd0-2` 3x11 MiB for Wi-Fi offload. *(Pass 2, now **[V]**:)* `rdpa.ko` looks up rdp1 as `FPMPOOL_BASE_ADDR_STR` through `BcmMemReserveGetByName` (the FPM pool, made non-coherent with UBUS decode window 1) and rdp2 as `RDP_PARAM2` (flow/NATC tables). Host packet buffers come from BPM, not rdp1 (doc 11 §3.4).
- `RDPA_MW: scratchpad get_or_init: RdpaWanType - GBE / WanOEMac - EMAC4 / GbeWanEMac - NONE`, then `rdpa_mw_sys_init.c rdpa_init_system Init Success`.
- **Runner CPU ring ownership (pass 2, [V] unless noted)**. Sources: creation order in `kernel.log`, `rdd_ring_init` types and the `wfd`/`dhd` log lines around each ring; details in doc 11 §4.2.

  | Ring | Entries x size | Owner | Role |
  |---|---|---|---|
  | 16 | 8192 x 8 B | `rdpa.ko` system init (logged right after `Configuring PCM bus mux`, before any netdev) | feed ring: empty BPM buffers for the Runner |
  | 17, 18 | 16384 x 8 B | `rdpa.ko` | recycle rings (TX-completed buffers back to the host); 17 on the image-1 core, 18 on the image-2 core |
  | 19, 20 | 2048 x 16 B | `rdpa.ko` | CPU TX rings, shared by **every** sender that calls `rdpa_cpu_send_sysb`: bcm_enet, wfd/dhd, bcmgpon |
  | **3, 4** | 1024 x 16 B | **`bcm_enet`** (`enetxapi_queues_init`) | Ethernet RX low/high priority. Interrupt bits 3/4 of `UBUS_SLV.RNR_INTR_CTRL` (interrupt id = ring id) |
  | 7+8, 10+11, 13+14 | 1024 x 16 B | **`wfd`**, one pair per radio (`wfd_bind: Dev wl0 … qmask 0x3`, then rings 7/8; likewise wl1, wl2) | Runner → WLAN forwarding RX |
  | 9, 12, 15 | 128 x 16 B | `dhd` per radio (after `Runner DHD PCIE: … bus<n>`) | DHD CPU queue |

  So bcm_enet owns only rings 3 and 4. Wi-Fi traffic that the Runner forwards to the host never passes through bcm_enet. An open Ethernet driver only has to recreate 3/4 (or one ring) plus the feed, recycle and TX rings.
- bcm_enet RX: `Creating CPU ring for queue number 4 with 1024 packets … size_of_entry 16`, the same for queue 3, then `Creating dynamic meter on cpu/index=0 queue 3`.
- `FPM group configuration set: DS = 20740, US = 40260 …`.
- `ERR: rdpa_if_to_rdd_vport#2522: Can't map rdpa_if 30 to rdd vport`. This is harmless. In the GPL `rdpa_types.h` enum without G9991, `rdpa_if 30 = rdpa_if_switch`, and this board has no external switch.
- `BCMENET: Enabling Software GSO for eth0..eth4`.
- `Pkt HW acceleration enabled` once pktrunner is loaded.

### 4.3 rdpa interface numbering (GPL `rdpa_types.h`, 5.02L.07) [V-source]
`rdpa_if_wan0..2 = 0..2`, `lan0..lan21 = 3..24`, `lag0..4 = 25..29`, `switch = 30`, …

`rdpa_wan_type_to_if()` on PON platforms: GPON/XGPON/EPON/XEPON → `wan0`. `gbe` → `wan0` as well, unless `CONFIG_MULTI_WAN_SUPPORT` is set, in which case it maps to `wan1`. *(Pass 2: **[V absent]**)* There is no `MULTI_WAN` string in `kernel.config`, `syms.txt`, `bcm_enet.ko`, `rdpa.ko`, `rdpa_mw.ko` or `rdpa_gpl.ko`, so **PON WAN and eth4-WAN are mutually exclusive in this build, chosen at boot**. `rdpa_mw` `wan_scratchpad_get` latches one WAN type at insmod (see §5.1).

impl7 `runner.c` (GPL, 5.02L.07p2): a port becomes WAN (`rdpa_if_wan0`) if `port == init_cfg.gbe_wan_emac`. Otherwise it is `rdpa_if_lan0 + port`. Here `GbeWanEMac=NONE`, so eth4 is first created as lan4 and later switched to WAN by `wanconf` and the HAL (`SIOCSWANPORT`, `port_role_set`). **[I]** This follows from the wanconf strings in section 5.

### 4.4 Load order (stock) [V]
`/etc/rc.d/S11bcmdrivers` runs `/etc/bcm-base-drivers.sh start`:
```
bcmlibs  wlcsm  bdmf  opticaldet  bcm_pondrv  rdpa_gpl  rdpa_gpl_ext  rdpa  rdpa_usr  rdpa_mw
(license: cp calib:/bcm_license.txt -> /proc/driver/license)  bcmbrfp  bcm_license  bcm_spdsvc  bcm_tcpspdtest
chipinfo  pktflow  bcm_enet  cmdlist  pktrunner  bcmmcast  (SATA...)  load_kernel_nvram.setting
bcm_pcie_hcd  wfd  hnd  emf  igs  cfg80211  dhd  wl  bcmvlan  pwrmngtd  i2s  rdpa_cmd
/etc/rdpa_init.sh  (ifconfig eth0 down; cat /dev/rgs_logger &; bdmf_shell -c init -> /var/bdmf_sh_id; alias bs=...)
```
Then `S15wanconf` runs `/usr/bin/wanconf` (WAN type, loads pmd/gpon/epon modules, converts EMAC to WAN). `S90rdpaconf` runs:
```
bs /b/c iptv {lookup_method=group_ip_src_ip}
fc config --accel-mode 0            # L2 accel disabled (Sagemcom comment: L3 DS HW accel broken with L2 on L05)
fc config --tcp-ack-mflows 1
fc config --mcast-learn 0
bs /d bbh s pkt_size0 wan 48 1536   # WAN BBH min/max packet size (1536 => no jumbo on WAN)
```
Also **[V]**: `pktflow` (fcache) is loaded before `bcm_enet`, and `bcm_enet` imports `blog_*`.

Userspace is **32-bit ARM EABI5 glibc** on a 64-bit kernel (`file usr/bin/ethctl` gives `ELF 32-bit LSB executable, ARM, EABI5 … /lib/ld-linux.so.3`). `CONFIG_COMPAT=y`.

---

## 5. WAN configuration on stock firmware

### 5.1 Scratchpad (Broadcom PSP) [V]
- `conf/scratchpad` (`ubi/fs_filesystem1/conf/scratchpad`): `GbeWanEMac=NONE`, `linkState=standby`, **`RdpaWanType=GBE`**, `WanRate=0101`, **`WanOEMac=EMAC4`**. This matches every logged boot.
- `fs_overlay/scratchpad`: `GbeWanEMac=NONE`, `SwImgValid=10`, `linkState=standby`, `WanRate=1010`, `RdpaWanType=XGS`, plus `tr69c_acsState`.
- *(Pass 2)* **Which copy is live.** Doc 03 §2.2 classifies the overlay copy as the **factory** copy and `conf/scratchpad` as the **live** copy. All 9 logged boots print `RdpaWanType - GBE / WanOEMac - EMAC4`, which matches the conf copy **[V]**. The precise kernel read path (`kerSysScratchPadGet`) is the boot/flash agent's (doc 01). Bell's `libbellosm-smu.so` (doc 03 §2.3) rewrites the value itself: with `WanMode=ftth` the box stays GBE; if fibre light is detected it can switch to GPON/XGSPON and reboot.
- **Ethernet consequences of the two settings** **[V]** for the code paths (`rdpa_mw.ko` `wan_scratchpad_get`, `rdpa.ko` `data_path_init_gbe`/`_fiber`, impl7 `runner.c`), **[I]** for the end-to-end behaviour:

  | | `RdpaWanType=GBE`, `WanOEMac=EMAC4` (live, logged) | `RdpaWanType=XGS`, `WanRate=1010` (factory copy) |
  |---|---|---|
  | RDPA system WAN | `rdpa_if_wan0` = EMAC4; `data_path_init_gbe(4)` points the upstream TM at BBH 0 (`rdpa_emac_to_bb_id_tx[4] = 32`) | `wan0` = XGS-PON MAC on the 9th BBH pair (`WANBBH` 0x80170000); `data_path_init_fiber` |
  | eth4 | Created as lan4, then turned into the WAN port by `wanconf` (`net_port` ioctl). Carries VLAN 33/34/35/36 and PPPoE | Stays a plain **LAN** port (10G LAN), bridged into BR_LAN like PHY5 |
  | Modules | no PON stack (pmd/ngponstack not loaded; `bcm_pondrv` still required by `rdpa`) | `pmd.ko`, `ngponstack.ko`, `bcmgpon.ko`; optics GPIO 60 = 0 (XGS) |
  | `S90rdpaconf` `bs /d bbh s pkt_size0 wan 48 1536` | applies to the eth4 BBH [I] | applies to the PON BBH [I] |
  | Open driver | LPORT 4 = WAN netdev. Nothing PON-related is needed | Not reproducible without a PON stack (doc 08). An open Ethernet driver ignores the scratchpad |

  A fresh unit, or one whose live scratchpad is reset to the factory copy, therefore boots with eth4 as a **LAN** port. To keep eth4 as the WAN on the stock stack, `RdpaWanType=GBE` and `WanOEMac=EMAC4` must be in the live copy, and Bell's auto-sensing must not override them (doc 03 §2.3).

### 5.2 wanconf (`usr/bin/wanconf`, 32-bit, stripped) [V strings]
- Reads PSP `RdpaWanType`, one of `AUTO|GBE|EPON|GPON|XGS|XGPON1|NGPON2|NONE`, and `WanRate`, one of `1010|1025|1001|0202|0201|0101`.
- For PON types it loads `pmd.ko`, `gponstack/ngponstack`, `bcmgpon.ko` or `bcmepon.ko`, sets the EponMPCP kthread to RR, and uses `/dev/wantypedetect` / `wantypedet.ko` for AUTO.
- For `GBE` it reads `WanOEMac`, which is `EMACn` (an LPORT MAC, here EMAC4 = eth4) or `EPONMAC` (Active Ethernet through the SFP and EPON MAC). It then calls the `bcmsw` ioctl: `"wanconf: net_port: port=%d sub_type=%d speed=%d"`, `"Failed to delete LAN port"`, `"Failed to create port"`.
- It also calls `rdpaCtl_time_sync_init`, `set_iptv_lookup_method` and `set_car_mode` through `/dev/rdpa_user`.
- `rdpa_mw.ko` strings show that a bad `WanOEMac` falls back to "Safe recovery mode to GBE EMAC4".

### 5.3 Logical WAN interfaces (Bell profile) [V]
From `backup-cfg-Ethernet-16.xml`, `PPP-8.xml` and kernel.log:
- `ETHLNK_DATA` / `ETHLNK_SERVICES` on PHY6 (eth4).
- VLAN terminations **35 (VLAN_DATA → PPPoE `PPP_DATA`, MRU 1492)**, **36 (VLAN_SERVICES, IPTV)**, **33 (VLAN_RMAT)**. The log also shows `eth4.34`.
- `GPON_DATA` and `GPON_SERVICE` links hang off `OPTICAL0` and are used when the WAN is PON.
- Interfaces are built with `vlanctl`: `vlanctl --if-suffix . --mcast --if-create %s %d --if %s --set-if-mode-rg`, plus rx/tx tag rules (libhal-network strings). The log shows `BCMVLAN : eth4 mode was set to RG` and `eth4.35 MAC address set to <device-mac>`.
- Operator log: `WETH 0 WAN Ethernet connectivity has been established` on many boots.

### 5.4 LAN bridge
- **[V]** `Bridging-8.xml` defines `BR_LAN` with PHY1..PHY5 (PVID 5) plus the Wi-Fi SSIDs, and a `BR_GUEST` bridge.
- **[V]** Interfaces `eth0.BR_LAN` and `eth0.BR_DBS` exist (log: `netdev path : eth0.BR_LAN -> eth0`). They are vlanctl sub-interfaces per port. BR_DBS is a Sagemcom bridge, **[U]** purpose unknown.
- **[V]** HAL strings use `ovs-vsctl set Bridge BR_LAN …`, and `CONFIG_BCM_OVS=y`. The LAN bridge is Open vSwitch (OpenSync/Plume) in user space, with the `openvswitch.ko` module loaded if present.

### 5.5 Making eth4 the WAN in practice (stock stack) [I]
1. Set PSP `RdpaWanType=GBE` and `WanOEMac=EMAC4` (`pspctl set …`). This is exactly what `conf/scratchpad` already holds.
2. Reboot so that `rdpa_mw` creates the system object in GBE mode and `wanconf` turns EMAC4 into `rdpa_if_wan0`.
3. Create VLAN 35 with vlanctl (or a plain 8021q device on eth4, **[U]** whether that is accelerated) and run PPPoE on top.

PON then cannot be used at the same time (section 4.3).

---

## 6. Runtime log digest (kernel.log, kernel.log.0, kernel.log.1: 9 boots) [V]

| Item | Observation |
|---|---|
| Kernel | `Linux version 4.1.52-5.02L.07 (rmm-svce-urd2int05@compil-atr-2) (gcc version 5.5.0 (crosstool-NG crosstool-ng-1.23.0-317-g85b86d8)) #1 SMP PREEMPT Fri Apr 15 16:48:43 CEST 2022`. Build path `…/fw-scos/openwrt/build_dir/target-sagemcom_5689-bell_aarch64-scos-linux-gnu/linux-4.1.52-5.02L.07/bcm963xx/…`, so Sagemcom's own build system is OpenWrt-derived |
| WAN mode | every boot: `RdpaWanType - GBE`, `WanOEMac - EMAC4`, `GbeWanEMac - NONE` |
| PHY detect | every boot: `84891 B0 3590:5091 --> 0x1e`, firmware Blackfin B0 v02-02-06, CRC OK |
| eth0 | `EGPHY:GMII:0x1` Link Up 1000 FD (15x), Down (9x) |
| eth1 | `EGPHY:GMII:0x2` Up 1000 FD (2x) |
| eth2 | `EGPHY:GMII:0x3` Up 1000 FD (2x) |
| eth3 | `EGPHY:GMII:0x4` Up 1000 FD (1x) |
| eth4 | `EXT3:XFI:0x1e` Link Up **1000 Mbps** FD (8x), **2.5 Gbps** FD (3x), Down (9x). 10G was never seen |
| 10G PHY control | at each link-up, libhal LED writes (`write45 4 0x1 0xA83B 0x8400 … 0xA83B 0x8410`). "reset 10G port" (`write45 4 0x1 0x0 0x8000`) about 3x per boot |
| VLAN | eth4.33/.34/.35/.36 created, mode RG. `rdpa_vlan_vid_set … Can't find RDPA vlan object for eth4.36/eth0.BR_DBS` (harmless) |
| Runner | CPU rings 3/4 (enet), 16-20. `Can't map rdpa_if 30` (switch). `Pkt HW acceleration enabled` |
| Promisc | `eth0/eth4 entered promiscuous mode` (bridging/OVS) |

---

## 7. Draft OpenWrt-style description (pseudo-DTS)

Addresses come from section 2.1. *(Pass 2:)* Interrupt numbers come from the GPL `6858_intr.h` (`INTERRUPT_LPORT_INTR_0_CPU_OUT = SPI_TABLE_OFFSET + 28`, `_1_ = +26`; XRDP queue bits 0-31 = SPI 128-159). The stock DTB has no nodes for them, so they are **[V-source]** but not hardware-tested. The PHY reset is GPIO80 (§1.3). Power domains need a `brcm,bcm6858-pmb` table that mainline lacks (doc 10 §3): LPORT = PMB bus 1 dev 9, XRDP = bus 1 dev 12-17.

```dts
/* Draft only - no upstream bindings exist for these blocks (see section 9). */
&soc {
    lport: ethernet-lport@80138000 {
        compatible = "brcm,bcm6858-lport";          /* hypothetical */
        reg = <0x0 0x80138000 0x0 0x6000>;          /* XLMAC/MIB/CTRL/SRDS/LED/RGMII/MDIO/INTR/MAB */
        interrupts = <GIC_SPI 28 IRQ_TYPE_LEVEL_HIGH>,  /* LPORT_INTR_0 (6858_intr.h) */
                     <GIC_SPI 26 IRQ_TYPE_LEVEL_HIGH>;  /* LPORT_INTR_1; trigger type [U] */
        power-domains = <&pmb 9>;                   /* hypothetical bcm6858-pmb, LPORT bus1/dev9 */

        mdio: mdio@8013d400 {
            compatible = "brcm,bcm6858-lport-mdio";  /* hypothetical; CMD @+0, CFG @+4 */
            reg = <0x0 0x8013d400 0x0 0x8>;
            #address-cells = <1>; #size-cells = <0>;

            gphy0: ethernet-phy@1 { reg = <1>; };   /* QEGPHY, C22 */
            gphy1: ethernet-phy@2 { reg = <2>; };
            gphy2: ethernet-phy@3 { reg = <3>; };
            gphy3: ethernet-phy@4 { reg = <4>; };
            phy10g: ethernet-phy@1e {
                compatible = "ethernet-phy-ieee802.3-c45";
                reg = <0x1e>;                       /* BCM84891 B0, id 0x3590:5091 (not 0x3590508x!) */
                firmware-name = "brcm/bcm84891_b0_v02-02-06.bin";  /* must be MDIO-loaded */
                reset-gpios = <&gpio2 16 GPIO_ACTIVE_LOW>;  /* GPIO80 "Phy rst all" (BSP + U-Boot hog) */
                /* GPIO4 "phy_enable" is hogged high by U-Boot; keep it as a gpio-hog */
            };
        };

        ports {
            port@0 { reg = <0>; label = "lan1"; phy-handle = <&gphy0>; phy-mode = "gmii"; };
            port@1 { reg = <1>; label = "lan2"; phy-handle = <&gphy1>; phy-mode = "gmii"; };
            port@2 { reg = <2>; label = "lan3"; phy-handle = <&gphy2>; phy-mode = "gmii"; };
            port@3 { reg = <3>; label = "lan4"; phy-handle = <&gphy3>; phy-mode = "gmii"; };
            port@4 { reg = <4>; label = "wan";  phy-handle = <&phy10g>;
                     phy-mode = "10gbase-r";        /* XFI; 2500base-x/1000base-x at lower rates [I] */
                     /* SerDes: LPORT Merlin16 core0 lane1 */ };
        };
        /* LED pins (LPORT LED block, active-low): p0 GPIO62(1G)/63(10/100), p1 65/66, p2 68/69, p3 71/72 */
    };

    xrdp: runner@82000000 {
        compatible = "brcm,bcm6858-xrdp";           /* hypothetical */
        reg = <0x0 0x82000000 0x0 0xe51000>,        /* XRDP */
              <0x0 0x80170000 0x0 0x4000>;          /* WAN BBH (PON only) */
        interrupts = <GIC_SPI 131 IRQ_TYPE_LEVEL_HIGH>, /* RNR_INTR_CTRL bit 3 = CPU RX ring 3 */
                     <GIC_SPI 132 IRQ_TYPE_LEVEL_HIGH>; /* bit 4 = ring 4 (doc 11 §4.4) */
        power-domains = <&pmb 12>, <&pmb 13>, <&pmb 14>, <&pmb 15>, <&pmb 16>, <&pmb 17>;
        memory-region = <&rdp1 /*64M FPM pool*/>, <&rdp2 /*8M tables*/>;  /* not needed for SBPM-only phase 1 */
        brcm,lport = <&lport>;
    };
};
```

| OpenWrt item | Value |
|---|---|
| Ports | `lan1..lan4` = eth0..eth3 (1G), `wan` = eth4 (10GBASE-T, 100M/1G/2.5G/5G/10G) |
| MACs | label/base `<device-mac>` (NVRAM mtd0 +0x106a0), WAN = base+1 |
| Bell WAN | VLAN 35 + PPPoE (MRU 1492). IPTV VLAN 36. RMAT VLAN 33 |
| PHY firmware | Blackfin B0 v02-02-06 (GPL-dual header in the Broadcom SDK; extracted blob in `work/eth/`) |

---

## 8. Source code availability

### 8.1 Sagemcom/Bell GPL
- **[U]** I found no public GPL tarball for the F@ST 5689 / Home Hub 4000. Sagemcom's old portal (opensource.sagemcom.com) is offline. A 2022 LKML thread complains about Sagemcom GPL compliance. The realistic route is a written GPL request to Bell or Sagemcom citing the device and firmware `4KOS_20.2.152` / kernel `4.1.52-5.02L.07`.

### 8.2 Same-SDK public trees (verified, cloned)
ASUS-Merlin mirrors Broadcom SDK drops. Sparse clone: `$P/work/eth/ref/merlin/`.
- `release/src-rt-5.02L.07p2axhnd` (its `version.make` says 5.02L.07, the same SDK as the HH4000), checked out in part:
  - `shared/opensource/boardparms/bcm963xx/boardparms_6858.c` and `include/bcm963xx/boardparms.h`, `bp_defs.h` (the stock enum; Sagemcom's differs, see 1.3).
  - `shared/opensource/drv/lport/*` (LPORT driver, Merlin direct access with its ucode image, 6858 AG register files) and `shared/opensource/drv/phys/*` (`phy_drv_6858_egphy.c`, `phy_drv_ext3.c` with the 84891 loader, `phy_drv_lport_serdes.c`, `mac_drv_lport.c`, `bus_drv_6858.c`, `phy_bp_parsing_6858.c`, `blackfin_b0_firmware.h`, …). All are labelled `DUAL/GPL`.
  - `rdp/drivers/rdpa_gpl/include/*` (rdpa API types).
  - **`rdp/projects/XRDP_CFE/` with `BCM6858_B0_*sources.list`, `drivers/rdd/BCM6858/*`, `drivers/rdp_subsystem/BCM6858/data_path_init.c`, `drivers/rdp_subsystem/xrdp/rdp_drv_{bbh_rx,bbh_tx,dma,dqm,fpm,qm,rnr,sbpm,cnpl,natc,tcam,dis_reor,psram,bac_if}.c`, `cpu/rdp_cpu_ring.c`, `firmware_bin/BCM6858/runner_fw_{0,1,2}.c` (Runner microcode as arrays)**. The headers are `DUAL/GPL` (checked on `data_path_init.c` and `runner_fw_0.c`).
    - Missing from this tree: `rdp/drivers/rdp_subsystem/BCM6858/autogen/XRDP_AG.h` / `XRDP_GPL_AG.h` (block addresses and field definitions), which ASUS does not ship because it does not build for 6858. *(Pass 2, resolved:)* the same Merlin repository's `release/src-rt-5.04behnd.4916/.../BCM6858/autogen/XRDP_AG.h` (DUAL/GPL) has all 969 registers and 2,957 fields, and they match the stock `rdpa.ko`. Base addresses come from `rdpa.ko` (`work/regdb/rdpa_fields.{h,json}`). The 5.04 trees also have the GPL U-Boot reduced-XRDP datapath for 6858 (doc 11 §1-2).
  - `kernel/dts/6858/96858.dts` (Broadcom legacy DTS).
- `release/src-rt-5.02axhnd.675x` (5.02L.07p2): **full bcm_enet impl7 sources** (`enet.c, port.c, runner.c, rdp_ring.c, runner_standalone.c, phy_drv_pon.c, mux_index.c, bp_parsing.c …`). The 5.02L.07p2axhnd tree ships only a prebuilt `bcm_enet` object.
- Not available: `rdpa.ko` / Runner full firmware / pktflow / pktrunner sources (proprietary).
- `rdpa.ko` symbols are a good RE reference for the XRDP AG layer: there are 1,366 `ag_drv_*` functions whose bodies encode register addresses.

### 8.3 Upstream status
*(Corrected in pass 2 from doc 10 §3-4 and §7 items 7, 16-18.)*
- **[V]** U-Boot mainline: the live target is **`bcm96858_defconfig`** (`TARGET_BCM96858`, OF_UPSTREAM, UART + brcmnand-bcmbca). The older `bcm968580xref.dts` is still built but has no defconfig of its own. There is no Ethernet, SPI, GPIO or USB support.
- **[V]** Linux mainline `bcm6858.dtsi` has CPU/GIC/timer/PMU, UART x2, GPIO x8, HSSPI, NAND, RNG, LED controller, PL081 DMA and a TWD/watchdog node (very likely at the wrong address, doc 10). It has **no** LPORT, MDIO, XRDP, PMB, I2C, USB or PCIe nodes. There is no LPORT/XRDP/Runner code anywhere in `drivers/net/ethernet/broadcom` (doc 10 §3, directory listing and commit search).
- **[V]** Linux commit `75171eeff353` "net: phy: bcm84881: add BCM84891/BCM84892 support" (Daniel Wagner, Apr 2026, now in torvalds master, plus an LED-framework follow-up). It supports **USXGMII host mode only** and does not load firmware: it assumes the PHY firmware is already running, as on the TEG-S750 test board.
  - **It does not even probe on this board.** It matches `PHY_ID_MATCH_MODEL(0x35905080)` (Broadcom's GPL table calls 0x3590508x "84891L") and `0x359050a0`. The HH4000 reports **0x35905091** ("84891 B0").
  - On the HH4000 the 84891 is wired for **XFI** and needs a **host MDIO firmware download**.
  - An OpenWrt driver therefore needs (a) a new ID entry for `0x35905090`, (b) a firmware loader ported from `phy_drv_ext3.c::load_blackfin` (halt, broadcast write, upload, reset, CRC check), (c) 10GBASE-R / 2500BASE-X / 1000BASE-X host-mode support with `CMD_SET_XFI_2P5G_5G_MODE`, and (d) the GPIO80 reset.
- **[V]** OpenWrt: no bcmbca/6858 target. OpenWrt's `bcm4908` target (kernel 6.18; BCM4906/4908 boards, one BCM4912 board defined but disabled) uses a different Runner (RDP, not XRDP), and its `bcm4908_enet` driver does not apply.

---

## 9. OpenWrt feasibility and recommendation

### (a) Mainline or modern kernel
Status: **No 6858 Ethernet support upstream.** Required work, roughly from easiest to hardest:
1. **LPORT MDIO bus driver** (C22/C45, register layout in section 3). Easy.
2. **QEGPHY bring-up.** Port `phy_drv_6858_egphy.c` plus LPORT CTRL/QGPHY power and base-address setup. Mainline `bcm7xxx`/`bcm-phy-lib` may cover the PHY registers once it is powered. **[U]** Not checked.
3. **XLMAC + LPORT mux + Merlin16 SerDes** (XFI, 2.5G, 1G) as a phylink PCS/MAC. Port about 9k lines of GPL `lport_drv.c`, `serdes_access.c`, `merlin_direct_access.c` and the Merlin ucode. Medium-hard.
4. **BCM84891 XFI + firmware loader**, on top of the new upstream bcm84881 driver: a new PHY ID entry (0x35905090), XFI/2500BASE-X host modes, the MDIO firmware download and the GPIO80 reset (§8.3). Medium. The firmware is GPL-dual, so it is redistributable.
5. **XRDP minimal datapath.** *(Updated pass 2; full spec in [11-xrdp-runner.md](11-xrdp-runner.md).)* Power XRDP through PMB, replay Broadcom's GPL U-Boot "reduced XRDP" init log for 6858 (a recorded register sequence including a 178-word Runner core-0 microcode), and move packets with the U-Boot SBPM polling RX/TX code. No proprietary blob is needed. Phase 2 ports XRDP_CFE `data_path_init.c` with its 3 GPL Runner images and DDR CPU rings for interrupt-driven I/O. The register/field header gap is closed (§8.2). Remaining gaps: FPM pool setup for phase 2, and throughput. The phase-1 path copies through SBPM with one TX descriptor in flight, so **[I]** expect tens to low hundreds of Mbit/s, far below 2.5G/10G. Doc 11 §7 estimates **3-5 months** for a first usable router (items 1-5 in phase 1 form).

There is no hardware NAT or flow offload on this path. pktflow/pktrunner are proprietary and tied to the 4.1 BCM kernel.

### (b) OpenWrt userspace on the stock 4.1.52-5.02L.07 kernel and stock modules
- vermagic is `4.1.52-5.02L.07 SMP preempt mod_unload aarch64` and `CONFIG_MODVERSIONS` is off. The modules depend on Broadcom-patched core kernel structures (blog/nbuff/skb fields, `bcm_sw_gso_xmit`, …), so **they load only into this exact kernel binary**, or into one rebuilt from the identical Sagemcom/Broadcom source and config.
- Kernel limits for current OpenWrt **[V config]**: no `CONFIG_NF_TABLES` (fw4/nftables will not work, use iptables/fw3 from OpenWrt ≤21.02 or a custom firewall), no namespaces (no ujail/procd seccomp jails), no `CONFIG_PHYLIB` / swconfig / DSA (the ports are plain netdevs, so netifd must treat eth0..eth4 as independent devices bridged in software or through `bcmvlan`).
- The Broadcom tools (`ethctl`, `bdmf_shell`, `wanconf`, `vlanctl`, `fcctl`/`fc`, `tmctl`, `pspctl`) are 32-bit glibc binaries with Broadcom/Sagemcom shared libraries. Run them in an armhf glibc compat area, or re-implement the few ioctls you need (bcmsw `SIOCETHSWCTLOPS`, `/dev/rdpa_user`, `/proc/driver/phy/cmd`).
- Minimum bring-up for Ethernet only (no Wi-Fi/voice), following the stock order: `bcmlibs bdmf opticaldet bcm_pondrv rdpa_gpl rdpa_gpl_ext rdpa rdpa_usr rdpa_mw pktflow bcm_enet [cmdlist pktrunner] bcmvlan rdpa_cmd`, then `bdmf_shell -c init`, then wanconf (or set the PSP in advance and just switch eth4 to WAN), optionally the `rdpaconf` `fc`/`bs` commands. **[I]** I have not tested whether `wlcsm`, `chipinfo` or `bcm_license` are needed. `rdpa` hard-depends on `bcm_pondrv`.
- This is the **fastest route to working 1G LAN and 10G WAN with hardware offload**, at the cost of an old, unmaintainable kernel.

### (c) GPL tarball
- Sagemcom/Bell: none found. Request it. Same-SDK public trees: available (section 8.2) and they cover enet impl7, lport, phys, boardparms, bdmf and rdpa_gpl headers, plus the XRDP_CFE minimal datapath for 6858.
- **[U]** Other vendors' BCM6858/68580 ONT/router GPL drops (Nokia, Zyxel, Askey, Sercomm, Actiontec, …) could supply the full `rdp/drivers/rdp_subsystem/BCM6858/` sources (`XRDP_AG.c`, per-block `xrdp_drv_*_ag.c`). I have not located one. *(Pass 2:)* This is no longer blocking: the 5.04 Merlin trees provide the 6858 `XRDP_AG.h` and the U-Boot reduced-XRDP datapath (§8.2).

### (d) Recommendation
1. **Phase 1 (pragmatic):** OpenWrt-style userspace (21.02-era, iptables) on the **stock Sagemcom kernel and stock BCM modules**. The OS agent will decide whether it ships as an image or as a chroot on the stock rootfs. Use PSP `RdpaWanType=GBE` and `WanOEMac=EMAC4` with eth4.35 PPPoE for Bell. This keeps Runner offload and is the only short-term way to reach 10G or 2.5G line rate.
2. **Phase 2 (long-term, mainline):** develop the drivers in order 1→5 from section 9(a). References: the GPL LPORT/PHY code, the GPL U-Boot reduced-XRDP datapath, the XRDP_CFE 6858 project, and the recovered register DB (doc 11). First milestone *(revised pass 2)*: "PMB power + MDIO + QEGPHY link on eth0 + replayed U-Boot XRDP init, ping over eth0". Second milestone: eth4 through Merlin16 XFI and the BCM84891 (doc 11 §2.5).
3. In parallel: send a GPL request to Bell/Sagemcom for the 4.1.52-5.02L.07 tree. With it, the kernel could be rebuilt (e.g. adding nf_tables) while keeping the stock binary modules.

---

## 10. Interfaces to other agents
- **PON agent:** PON and GBE share `rdpa_if_wan0` (no MULTI_WAN), chosen at boot by PSP `RdpaWanType`. `rdpa.ko` requires `bcm_pondrv.ko` loaded even in GBE mode. Active Ethernet over the SFP is `RdpaWanType=GBE` + `WanOEMac=EPONMAC` (wanconf loads `bcmepon.ko`). The WAN SerDes is `/proc/serdes/wan/*` (serdesctrl). The SFP GPIOs from boardparms are WanSignalDetected GPIO49 and OpticalModuleTxPwrDown GPIO55. `wanconf`'s `WanRate` codes are `1010, 1025, 1001, 0202, 0201, 0101`.
- **Boot/flash agent:** two scratchpad copies (conf=GBE, overlay=XGS). Doc 03 calls conf the live copy and the logs agree (§5.1). The exact kernel read path is still yours to confirm. NVRAM layout (board id at 0x10684, MAC count at 0x1069c, base MAC at 0x106a0 in mtd0).
- **Peripherals agent:** LED GPIOs 62/63/65/66/68/69/71/72 (LPORT LED). *(Pass 2)* GPIO80 "Phy rst all" (active-low) and GPIO4 `phy_enable` (U-Boot hog, high) belong to the 10G PHY (§1.3). GPIO4 can stay a gpio-hog (high). GPIO80 should be owned by the PHY driver as `reset-gpios`, not hogged. The boardparms table also lists I2C GPIOs (`BpGetI2cGpios` ids 0x72/0x73 = GPIO40/39; I2c2 = GPIO23/24), the UART2 pins, and a list of `bp_usGpioReserved` entries.
- **OS agent:** userspace is armhf glibc on an arm64 kernel. The kernel config is in `work/eth/kernel.config` (no nf_tables, no namespaces). The Sagemcom firmware is itself built with an OpenWrt-derived buildroot (`fw-scos/openwrt/build_dir/target-sagemcom_5689-bell_aarch64-scos-linux-gnu`).

---

## 11. Artifacts (`$P/work/eth/`)
| File | Content |
|---|---|
| `vmlinux.elf`, `syms.txt` | stock kernel with kallsyms (vmlinux-to-elf) and symbol list |
| `kernel.config` | embedded IKCONFIG |
| `bp_disasm.txt`, `bp_ids_from_code.txt`, `scripts/bpids.py` | boardparms accessor disassembly and id map |
| `bp_FAST5689_raw.txt`, `bp_968580XREF_raw.txt`, `scripts/dumpbp.py` | raw board tables |
| `bcm84891_blackfin_b0_v02-02-06.bin` | 84891 B0 firmware extracted from the kernel (identical to the GPL header) |
| `ref/boardparms*.{c,h}`, `ref/bp_defs.h`, `ref/merlin/` | Broadcom 5.02L.07 / 5.02L.07p2 reference sources (sparse clone of RMerl/asuswrt-merlin.ng) |

Ghidra *(updated pass 2)*: headless decompiles of `rdpa.ko` and `bcm_enet.ko` are in `work/eth/ghidra/dec/` (`Eth_DecompSel.java`). The XRDP register/field DB is `work/regdb/rdpa_fields.{h,json}`, the decoded U-Boot XRDP init is in `work/eth/xrdp/`, and `work/eth/scripts/intfdesc.py` dumps the libhal port tables (§2.4). See doc 11 §9.

## Sources (web)
- Linux net-next commit "net: phy: bcm84881: add BCM84891/BCM84892 support": https://kernel.googlesource.com/pub/scm/linux/kernel/git/mkl/linux-can-next/+/75171eeff3538c6859b50f6a516342c8f1ed6e00
- U-Boot BCM6858 bcmbca support: https://git01.mediatek.com/plugins/gitiles/filogic/uboot/+/a287017af059090dd1a379010f7b01e66d7b042e
- OpenWrt bcm63xx U-Boot tree (968580xref board): https://git.openwrt.org/project/bcm63xx/u-boot/atom/board/broadcom?h=master
- Sagemcom GPL-compliance LKML thread (2022): https://lkml.iu.edu/hypermail/linux/kernel/2205.3/05155.html
- ASUS-Merlin SDK mirror: https://github.com/RMerl/asuswrt-merlin.ng (`release/src-rt-5.02L.07p2axhnd`, `release/src-rt-5.02axhnd.675x`)

---

## Revision log (pass 2, 2026-11-04)
Every **[I]**/**[U]** item was re-checked against `bcm_enet.ko`/`rdpa.ko` decompiles, `libhal-network.so`, `bsp_gpio_lkm.ko`, the stock U-Boot DT, `kernel.log*`, doc 03, doc 04 and doc 10.

| Section | Change | New status |
|---|---|---|
| §0 table | Added the rear-panel jack and Runner BBH columns, the PHY ID and the GPIO80 reset | |
| §0, §8.2, §9(c) | The 6858 XRDP field header is no longer missing: 5.04 Merlin `XRDP_AG.h` and the U-Boot reduced XRDP (doc 11) | [V] |
| §1.3 | 84891 reset: **GPIO80 "Phy rst all", active-low** (BSP id 0x0ba + U-Boot hog `phy_reset`), GPIO4 `phy_enable` hog high. `bsp_reset_gpio_lan` pulse timing | [V] pins; [I] that they serve the 84891; [U] boot-time use |
| §2.2 | Mainline `bcm84881` matches 0x3590508x (84891L). This board's 0x35905091 does not bind (doc 10) | [V] |
| §2.3 | Per-jack LED table | [I] (inherits §2.4); colours still [U] |
| §2.4 | HAL tables prove PhyNum n = eth(n-1). Rear panel LAN 1-4 + separate 10G jack (FCC photo, doc 10) | [V] tables and photo; [I] silkscreen = PhyNum |
| §3 | No mainline LPORT MDIO driver | [I] → [V] (doc 10) |
| §4.2 | rdp1 = FPM pool, rdp2 = `RDP_PARAM2` tables | [I] → [V] |
| §4.2 | CPU ring ownership: rdpa 16-20, **bcm_enet 3/4**, wfd 7/8, 10/11, 13/14, dhd 9/12/15 | [U] → [V] |
| §4.3 | No `MULTI_WAN` anywhere | [I] → [V absent] |
| §5.1 | Live scratchpad = conf copy (doc 03 + 9 logged boots). Table of GBE vs XGS Ethernet consequences, Bell auto-sensing caveat | [U] → [V] for the logged boots; precedence path stays with doc 01 |
| §7 | LPORT and XRDP interrupt numbers (`6858_intr.h`), PMB power domains, `reset-gpios` | [V-source], untested |
| §8.3 | Applied doc 10 corrections #7, #16, #17 and #18 (U-Boot `bcm96858_defconfig`, current dtsi contents, PHY ID mismatch, OpenWrt bcm4908 scope) | [V] |
| §9(a) | Items 4 and 5 rewritten around the PHY ID gap and the GPL U-Boot XRDP path. Effort pointer to doc 11 §7 | [I] estimates |
| §11 | Ghidra/regdb/xrdp artefacts now exist | |

Unchanged and still open: **[I]** no PHY SPI flash (9/9 MDIO uploads), **[I]** 1000BASE-X/2500BASE-X on the MAC side at 1G/2.5G, **[I]** eth4 created as lan4 and converted by `wanconf`, **[U]** jack LED colours, **[U]** BR_DBS purpose, **[U]** whether plain 8021q on eth4 is accelerated, **[U]** whether mainline `bcm7xxx` drives the QEGPHY, **[U]** which stock modules (`wlcsm`, `chipinfo`, `bcm_license`) are needed for Ethernet only, **[U]** Sagemcom GPL tarball.
USB (doc 10 #5: USB-A + USB-C) does not affect this document.
