# 10 - Physical hardware identification and upstream (Linux / OpenWrt / U-Boot) status

Research date: 2026-11-03/04. Scope: what the board physically contains (FCC/ISED filings, photos),
and what current mainline Linux, OpenWrt and U-Boot support for each BCM6858 block, checked
against our local evidence.

Legend: **[V]** verified (link, fetched source file, or local file/log line), **[I]** inferred,
**[U]** unverified / not visible. `$P` = `/path/to/HH4000-OpenWRT`.

Local artefacts from this pass (all under `$P/work/hw/`):
- `fccid_VW3FAST5689.html`, `fccid_VW3FAST5689E.html`: fccid.io listing pages.
- `fcc/*.pdf`: HH4000 exhibits (internal/external photos, label, manual, test reports, MPE) and
  Giga Hub internal/external photos. `fcc/raw/*.jpg` are the extracted photos (`hh-*` internal,
  `ext-*` external), `fcc/graw/` the Giga Hub ones, `fcc/*.txt` the test-report text.
- Mainline source files fetched for this doc were read from `raw.githubusercontent.com/torvalds/linux/master`
  (and the matching U-Boot / OpenWrt trees) on 2026-11-03.

---

## 0. Summary

- **FCC ID VW3FAST5689** is the HH4000 ("Home Hub 4000(FAST 5689)"), granted 2021-01-04 by SGS,
  grantee code **VW3** = Sagemcom Broadband SAS [V]. **ISED: IC 9140A-FAST5689** [V label PDF].
  **VW3FAST5689E is the Giga Hub**, not the HH4000 [V]. The Giga Hub board is visibly the same PCB
  family (same SoC/DRAM/BOSA/port layout) with a 6 GHz radio [V photos, I for chip IDs].
- **Surprise:** the HH4000 FCC filing certifies **Zigbee (802.15.4, 2405-2480 MHz, O-QPSK)** and
  **Z-Wave (908.4/916 MHz, FSK)** transmitters besides WiFi, and the internal photos label
  "Z-wave" and "ZB/5G ANT" antennas [V]. The stock BSP has "Spi Cs Zigbee / Reset Zigbee /
  Int Zigbee" names but **no Zigbee GPIO is assigned on the Bell board table** and nothing in the
  rootfs drives such a radio [V]. Whether production units have these radios populated is [U].
- Photos confirm: **4x yellow LAN + 1 separate 10G RJ45, 2x RJ11 TEL, barrel PWR**, **two USB
  receptacles on the side (one USB-C, one USB-A with a blue USB3 tongue)**, a rear-ish RESET
  button, front OLED, **one SC/APC fibre connector feeding a single BOSA** [V]. 12 V / 5 A external
  PSU (MSA-Z5000IS12.0-60A-P) [V].
- Chip markings are **not readable** in the FCC photos (1109x740 JPEGs). Visible: one lidded
  Broadcom SoC with **two DRAM packages** beside it, three WiFi SoCs (two top, one bottom), TDK
  LAN magnetics. Identifications therefore rest on our logs [V] rather than photos.
- **Mainline Linux for BCM6858 (master, Nov 2026)**: CPU/GIC/timer/PMU, UART, GPIO (8 banks),
  HSSPI, NAND, RNG, LED controller and PL081 DMA nodes exist. **No I2C, USB, PCIe, PMB, pinctrl,
  Ethernet, PON or WiFi support.** Two concrete problems found:
  1. The mainline dtsi **watchdog at 0xff800428 is very likely wrong**: the vendor
     `6858_map_part.h` puts the 6858 timer block at **0xff802700** and WDTIMR0 at **0xff802780**,
     the same address the stock DT and `hard_watchdog.ko` use [V]. The 0x400 "TWD" node looks
     copied from BCM4908.
  2. **BCM84891 B0 on this board reports PHY ID 0x35905091**. Mainline `bcm84881.c` matches
     `0x35905080` (named "BCM84891", but Broadcom's GPL table calls 0x3590508x the **84891L**) and
     `0x359050a0`. With the model mask, 0x35905091 does **not** bind. The driver also only accepts
     USXGMII and does not load firmware [V].
- **HSSPI on 6858 uses `spi-bcm63xx-hsspi.c`** (`brcm,bcmbca-hsspi-v1.0`), **not**
  `spi-bcmbca-hsspi.c` (v1.1 only) [V]. Doc 04 named the wrong driver.
- **`i2c-brcmstb` (`brcm,brcmper-i2c`) matches the 6858 I2C register layout exactly** (offsets
  0x00-0x54, the same CTLHI bit 6 data-width flag that the stock `bcm63000_xfer` tests) [V].
  It needs a supported `clock-frequency` (100 kHz is not in its table) [V].
- **SSD1322 in mainline `ssd130x` is wrong for this panel**: confirmed, by source and by the
  SSD1322 command table (0xA8 is "enable partial display", MUX is 0xCA, contrast 0xC1, and data
  needs 0x5C first; ssd130x sends none of that correctly) [V].
- **brcmfmac has no BCM43684 (0xaaa4)** [V]. **OpenWrt has no bcmbca/6858 target**; `bcm4908`
  (kernel 6.18) covers BCM4906/4908/4912 boards only [V]. **U-Boot mainline has
  `bcm96858_defconfig`** (UART + NAND only, OF_UPSTREAM) [V].
- No public HH4000 UART pinout, teardown with readable chip markings, OpenWrt attempt or GPL drop
  was found. Community work is limited to ONT masquerading (pon.wiki/WAS-110) and API clients
  (python-sagemcom-api, pgaskin/lua-sagemcom-openwrt) [V].

---

## 1. Regulatory filings

| Item | Value | Tag / evidence |
|---|---|---|
| Grantee code | **VW3** = SAGEMCOM BROADBAND SAS (FRN 0017082165) | [V] [fccid.io/VW3](https://fccid.io/VW3) |
| HH4000 FCC ID | **VW3FAST5689**, "Home Hub 4000(FAST 5689)", Original Equipment 2021-01-04 (application 2020-12-23), TCB SGS North America, test lab SGS-CSTC Shenzhen, reports SZEM2009008718xx | [V] [fccid.io/VW3FAST5689](https://fccid.io/VW3FAST5689), local `work/hw/fccid_VW3FAST5689.html` |
| HH4000 ISED | **IC: 9140A-FAST5689**, HVIN "Home Hub 4000(FAST 5689)" | [V] label exhibit `fcc/hh4k_label.pdf` |
| Rating | 12 V 5 A | [V] label, test reports |
| PSU | Sagemcom "Switching Power Adaptor" **MSA-Z5000IS12.0-60A-P**, 100-120 V~ 1.5 A in, 12 V 5 A out, barrel plug, right-angle | [V] `hh4k_test_5053487.txt` §4.1, photos `hh-023/027/028.jpg` |
| Radios certified | 2.4 GHz 11b/g/n/ax (2412-2462); 5 GHz U-NII-1/2A/2C/3, "802.11a/n/ac/ax 4x4 MIMO", 20/40/80/160 MHz; **2405-2480 MHz O-QPSK (Zigbee/802.15.4), 0.078 W, ant 3.85 dBi**; **908.4/908.42/916 MHz FSK (Z-Wave)** | [V] grant notes, `hh4k_test_5053430.txt` (Zigbee), `hh4k_test_5053487.txt` (Z-Wave) |
| Schematics / block diagram / operational description | **Confidential** (metadata only, long-term confidentiality) | [V] listing |
| Giga Hub | **VW3FAST5689E** "Giga Hub" (2022-03-08, later C2PC 2022-06/08), adds 5955-7095 MHz (6 GHz), MRT Suzhou lab. Also has Zigbee + Z-Wave. A schematic exhibit is named "FAST5689E_v2_QPF7219" (Qorvo QPF7219 is a 2.4 GHz WiFi FEM) | [V] [fccid.io/VW3FAST5689E](https://fccid.io/VW3FAST5689E); QPF7219 identity [I] |
| Other Bell names | python-sagemcom-api lists F@st 5689 = Bell Home Hub 4000 (md5 auth), F@st 5689E **and F@st 5690** = Bell Giga Hub (sha512) | [V] [README](https://github.com/iMicknl/python-sagemcom-api) |

---

## 2. Physical hardware (FCC internal/external photos)

Photos: `$P/work/hw/fcc/raw/hh-004.jpg` (rear), `hh-007/008/011.jpg` (board in chassis, heatsinks),
`hh-016/019.jpg` (top side, shields lifted), `hh-020.jpg` (bottom side), `ext-013/016.jpg` (sides),
rendered pages `fcc/img/int-5.png`, `int-6.png` (antenna labels).

| Item | What the photos show | Status |
|---|---|---|
| Rear panel | Left to right: **PWR** barrel, **TEL 1, TEL 2** (green RJ11), **LAN 1-LAN 4** (yellow RJ45, 2x2 ganged), **one separate silver RJ45** whose label ends in "10G" | [V] `hh-004.jpg` (label of the 10G jack only partly legible [I]) |
| Side ports | **USB-C** receptacle and **USB-A** receptacle with blue (USB 3) tongue, side by side | [V] `ext-013.jpg`, also visible at the board edge in `hh-016/019.jpg` |
| Other side | **RESET** pin-hole button; the front display/button strip | [V] `ext-016.jpg` |
| Fibre | **One** green SC/APC adapter, one fibre pigtail into **one** cylindrical optical sub-assembly with a gold flex, inside its own shield frame | [V] `hh-016/019.jpg`. Resolves 07 §8 Q8: a **single combo BOSA** [I that it carries both GPON and XGS-PON wavelengths, since both M02099 and M02180 drivers are on the board] |
| SoC | One large **lidded Broadcom package** (Broadcom logo visible, part text unreadable) | [V] photo, [V] logs: `License chip name: 68580X`, PCIe RC `[14e4:6858]`, CPU `[420f1000]` = Brahma-B53 r0p0 |
| DRAM | **Two** BGA DRAM packages next to the SoC, same shield | [V] photo; size 1 GiB [V] `Memory: .../941052K` and `totalpages 235263`; per-chip 2x 4 Gbit [I]; type/vendor [U] |
| NAND | Not identifiable in photos | [V] logs: `Manufacturer ID: 0x01, Chip ID: 0xdc`, "AMD/Spansion S34ML04G2", 512 MiB SLC, 2 KiB page, 128 KiB block |
| WiFi | Three shielded RF areas with a large BGA each: two on the top side, one on the bottom side; many antenna feed pads. Antennas labelled 2.4G_1..3, 2.4/5G ANT, 5G1..5G6, 2x ZB/5G ANT (12 printed/flex antennas) | [V] photos/labels (`int-5.png`, `int-6.png`); 3x BCM43684 [V] logs `[14e4:aaa4]` on 0000/0001/0002:01:00.0, `chipnum 0xaaa4, corerev 129`, fw `43684b0/rtecdc.bin`. 4+8 antenna count matches 1x 4x4 2.4 GHz + 2x 4x4 5 GHz [I] |
| FEMs | Not legible | [U] |
| 10G PHY | Not legible (under heatsink/shield) | [V] logs: `84891    B0 3590:5091 --> 0x1e`, `Firmware version: Blackfin B0 v02-02-06` |
| LAN magnetics | Two **TDK** dual magnetics for LAN1-4 plus a third transformer next to the 10G jack | [V] photo (TDK logo legible), part numbers [U] |
| Optics drivers | Not legible | [V] logs: `Part Number : M02099`, `Part Number : M02180`, `Part REV : 3`, `m02099 0-004e`, `m02180 1-004e` |
| SLIC | No separate daughterboard visible; the TEL area is on the main PCB | [V] logs: `voice board [F@ST_5689] daughter board [SI32282_CB]` (Broadcom's voice-board ID naming, not a physical daughterboard [I]) |
| MCU / FUSB301 / TMP102 | Not legible | [V] logs: `mcu-i2c 0-0078: mcu detected f5689-mcu`, `fusb301_update_reg(...)`, TMP102 nodes @0x48/0x49 in stock DT. EFM8BB1 identity rests on doc 04's firmware analysis |
| Zigbee / Z-Wave | Antennas labelled "Z-wave" (bottom side, near the USB corner) and "ZB/5G ANT"; certified transmitters | [V] FCC. Radio chips [U]; populated on retail units [U]; no driver/daemon in stock rootfs [V] (`grep` of rootfs: only `libhal-bsp` getters `getMacZigbee1/getZigbeeSSID1`, `bsp_gpio_lkm` name strings, and `libdevice-info-dm` labels) |
| OLED | Small dark-blue window panel with flex at the front edge | [V] `hh-007/011.jpg`; SSD1322 [V] logs `SSD1322 screen: width=167 (upscaled to 168) height:42` |
| Cooling | Two large finned heatsinks (top and bottom), spring screws, no fan | [V] photos |
| UART header | **Not identified** in the FCC photos (resolution too low to read silkscreen) | [U]. The owner's Arduino method proves a 115200 8N1 console exists |
| PSU internals | Single-board flyback, ~12 V 5 A | [V] `hh-027/028.jpg` |

Giga Hub (FAST5689E) comparison [V `fcc/graw/g-020.jpg`]: same board outline, same SoC + 2 DRAM
placement, same single BOSA, same TEL/LAN/10G/USB arrangement; antenna labels add "6G Ant 0-3".
So the Giga Hub is a revision of the same platform [I]. Its 6 GHz radio chip is [U].

---

## 3. Mainline Linux status for BCM6858 blocks (torvalds/linux master, fetched 2026-11-03)

Mainline `bcm6858.dtsi` history: initial DTS 2022 (William Zhang), TWD block 2022-11 (Rafał
Miłecki), HSSPI 2023-02, NAND 2024-02, "Add BCMBCA peripherals" 2025-05 (Linus Walleij: GPIO,
RNG, LEDs, UART1, PL081) [V GitHub commit history]. The only board file is `bcm96858.dts`
(Broadcom reference, 128 MiB memory node, UART0 + HSSPI + NAND enabled) [V].

| Block | HH4000 hardware (stock) | Mainline driver / compatible | Status for 6858 | Evidence |
|---|---|---|---|---|
| CPU, GIC-400, arch timer, PMU, PSCI | 4x B53 | `brcm,brahma-b53`, `arm,gic-400`, `arm,armv8-timer` | **Supported** in dtsi | [V] bcm6858.dtsi |
| UART | `ttyS0` 0xff800640, `ttyS1` 0xff800660 | `bcm63xx_uart.c`, `brcm,bcm6345-uart` (Kconfig depends on ARCH_BCMBCA) | **Supported**, uart0/uart1 nodes | [V] dtsi, `drivers/tty/serial/bcm63xx_uart.c` |
| HS UART | 0xff858400 (PERF1+0x400) | none | **Missing** | [V] vendor `6858_map_part.h` `HS_UART_OFFSET 0x0400` |
| GPIO | DIR 0xff800500+4n, DATA 0xff800520+4n | `gpio-mmio.c`, `brcm,bcm6345-gpio` | **Supported**, gpio0-7 nodes (disabled by default) | [V] dtsi, `drivers/gpio/gpio-mmio.c` |
| Pinctrl | TEST_PORT at 0xff800554/558/55c, cmd 0x21 | `pinctrl-bcm4908.c` uses the same scheme at base+0x60 with offsets 0x04/0x08/0x0c, cmd 0x21 | **Missing for 6858**; a 6858 variant (base 0xff800550, own pin table) fits the existing driver | [V] `drivers/pinctrl/bcm/` (no 6858 file), offsets compared with doc 04 §4.3 |
| Watchdog | WDTIMR0 **0xff802780** (DefCount/Ctl/ResetCount/SoftRst/AccessCtl), 50 MHz | `bcm7038_wdt.c`, `brcm,bcm6345-wdt` (TIMEOUT 0x0, CMD 0x4, 0xff00/0x00ff start, 0xee00/0x00ee stop) | **Driver fits; the dtsi node is probably at the wrong address** (twd@ff800400/watchdog@28 = 0xff800428). It also has no clock, so the driver assumes 27 MHz | [V] dtsi; [V] vendor `TIMR_OFFSET 0x2700`, `WDTIMR0_OFFSET 0x2780`; [V] stock DT `watchdog@ff802780`; [V] `WDT_DEFAULT_RATE 27000000` in driver. Wrong address: [I], high confidence |
| Timer | 0xff802700, 64-bit TimerCtl/TimerCnt layout | `brcm,bcm63138-timer` at 0xff800400 in dtsi | **Probably wrong** (same copied TWD node). Harmless if unused | [V] vendor `typedef struct Timer` (uint64 regs); wrongness [I] |
| Reboot | WDTIMER0 `SoftRst` (0xff80278c) | 4908 uses `syscon-reboot` on TWD; 6858 dtsi has none | **Missing** (would need `syscon-reboot` on the 0xff802780 block) | [V] dtsi; [I] method |
| RNG | 0xff800b80 | `iproc-rng200.c`, `brcm,iproc-rng200` | **Supported** | [V] dtsi, stock DT identical |
| LED controller | 0xff800800 | `leds-bcm63138.c`, `brcm,bcm63138-leds` | **Supported** node (HH4000 front LEDs are plain GPIOs anyway, doc 04) | [V] dtsi, driver compat |
| HSSPI | 0xff801000, IRQ SPI 37 | **`spi-bcm63xx-hsspi.c`** matches `brcm,bcmbca-hsspi-v1.0` (and `brcm,bcm6328-hsspi`); `spi-bcmbca-hsspi.c` only matches `brcm,bcmbca-hsspi-v1.1` and needs a `spim-ctrl` reg | **Supported** via `CONFIG_SPI_BCM63XX_HSSPI` | [V] dtsi `"brcm,bcm6858-hsspi", "brcm,bcmbca-hsspi-v1.0"`; both driver match tables |
| NAND | brcmnand v7.1 @0xff801800 + int 0xff802000, S34ML04G2, BCH-4 | `bcmbca_nand.c` (`brcm,nand-bcm63138`) + `brcmnand.c` | **Supported** | [V] dtsi, `drivers/mtd/nand/raw/brcmnand/` |
| I2C | `brcm,bcm63000-i2c` @0xff85a800 (TMP102 x2, M02099, MCU 0x78, FUSB301 0x25) and @0xff802100 (M02180) | `i2c-brcmstb.c`, `brcm,brcmper-i2c` (1-byte data regs, polling if no IRQ) | **No node in 6858 dtsi; driver register layout matches exactly** (chip_address 0x00, data_in 0x04-0x20, cnt 0x24, ctl 0x28, iic_enable 0x2c, data_out 0x30-0x4c, ctlhi 0x50, CTLHI bit 6 = 4-byte regs). Valid `clock-frequency` values: 375000, 390000, 187500, 200000, 93750, 97500, 46875, 50000 | [V] vendor `typedef struct I2CControl`, `i2c-brcmstb.c` `struct bsc_regs` and `bsc_clk[]`; stock `bcm63000_xfer` reads +0x50 and tests bit 0x40 (disasm at 0xffffffc000469dac..ddc). Untested on hardware [U] |
| TMP102 | 0x48/0x49 | `hwmon/tmp102.c`, `ti,tmp102` | **Supported** (needs the I2C node) | [V] driver |
| USB | USB_CTRL 0x8000c200, EHCI/OHCI 0x8000c300/c400 (+c500/c600), xHCI 0x8000d000 | `phy-brcm-usb.c` `brcm,bcm4908-usb-phy` + generic-ehci/ohci/xhci; BCM4908 uses the **same addresses** | **No 6858 node; plausible reuse of the 4908 binding.** Needs the PMB power domain (below) | [V] bcm4908.dtsi `usb-phy@c200`, `usb@c300/c400/d000`; [V] vendor `USBH_PHYS_BASE 0x8000c000`, `CFG 0x200`, `XHCI 0x1000`; reuse [I] |
| PMB / power domains | PMC 0x80200000; 6858 PMB devices: USB30_2X bus1/dev10, PCIE0 bus1/dev18, PCIE1 bus1/dev19, SATA bus1/dev20, LPORT bus1/dev9, XRDP bus1/dev12.., WAN bus1/dev11 | `pmdomain/bcm/bcm-pmb.c` has only `brcm,bcm4908-pmb` and `brcm,bcm63138-pmb` (4908 USB = bus1/dev17) | **Missing**: needs a `brcm,bcm6858-pmb` table | [V] driver match table; [V] vendor `pmc/pmc_drv.h` lines 775-912 |
| Clocks | periph 200 MHz fixed, HSSPI PLL 400 MHz | fixed-clock only | Fixed clocks only. 50 MHz WDT/timer clock not described | [V] dtsi |
| PCIe (3x Gen2 x1, Core Rev 3.20, RC `[14e4:6858]`) | 0x80040000/50000/60000, MEM 0xc0/0xd0/0xe0000000 | `pcie-brcmstb.c` has `brcm,bcm4908-pcie` | **Missing for 6858**; the brcmstb driver with a 6858 config + PMB + PERST is a plausible path | [V] driver match table; [V] logs `bcm963xx-pcie: found port [n] GEN2 Core Rev [3.20]`; path [I] |
| Ethernet (LPORT/XLMAC, QEGPHY, Runner/XRDP) | eth0-4 via XRDP | `bcm4908_enet.c` is the 4908 ENET block only; no LPORT/XRDP/Runner code anywhere in `drivers/net/ethernet/broadcom` | **None** | [V] directory listing; GitHub commit search "bcm6858" returns only DT/binding commits |
| BCM84891 10GBASE-T | PHY ID **0x35905091** (B0), XFI host, MDIO firmware download | `net/phy/bcm84881.c`: BCM84891 = `PHY_ID_MATCH_MODEL(0x35905080)`, BCM84892 = `0x359050a0`; `bcm8489x_config_init` returns -ENODEV unless `PHY_INTERFACE_MODE_USXGMII`; no firmware loader | **Will not bind as-is.** Needs (a) an ID entry for 0x35905090 (Broadcom's GPL table: 0x3590509x = "84891", 0x3590508x = "84891L"), (b) 10GBASE-R/XFI (+2500BASE-X/SGMII) host modes, (c) the firmware loader | [V] driver source; [V] log `84891    B0 3590:5091`; [V] `phy_drv_ext3.c` ID table in the ASUS-Merlin tree (`{0x3590,0x5091,"84891 B0"}`, `{0x3590,0x5081,"84891L B0"}`) |
| OLED SSD1322 | HSSPI CS5, 168x42, column offset 40, 4-px column units | `drm/solomon/ssd130x*.c`, `solomon,ssd1322` → SSD132X family | **Wrong command set** (see §3.1) | [V] source + datasheet table |
| WiFi BCM43684 | `[14e4:aaa4]`, dhd/rtecdc "fdap" firmware | `brcmfmac`: no 43684 chip ID, no 0xaaa4 PCI ID, no fw entry. Nearest supported: 4366 (BCA vendor) | **Not supported** | [V] `brcm_hw_ids.h`, `brcmfmac/pcie.c` |
| PON (GPON/XGS MAC, M02099/M02180) | | none | **None** | [V] no driver; see 03/08 |
| SLIC / PCM | Si32282 on HSSPI, PCM @0xff860000 | none for BCA | **None** | [V] no driver |
| FUSB301 | I2C 0x25 | none in mainline (`typec/tcpm/` has fusb302 only) | **Missing**; userspace `i2cset` (doc 04) | [I] (fusb302 is a different, PD-capable part) |
| EFM8 MCU | I2C 0x78 | none | **Missing** (custom) | [V] |
| Crypto (SPU/PDC), PKA, OTP | 0x8001c000/d000, 0xff800c00.. | `bcm-spu`/`bcm-pdc` exist for iProc; not wired for BCA | **Not supported** [I] | stock DT `brcm,pdc`, `brcm,spu-crypto` |

### 3.1 SSD1322 vs mainline ssd130x [V]

`ssd132x_init()` (master) sends: `81 80` (contrast), `15 00 w/2-1`, `75 00 h-1`, `A0 53`, `A1 00`,
`A2 00`, `A4`, `A8 h-1`, `B1 55`, `B9`, `B3 01`, `AB 01`, `B6 01`, `BC 08`, `BE 07`, `D5 62`.
`ssd132x_update_rect()` uses `SSD132X_SEGMENT_WIDTH = 2` and writes pixel data with no preceding
command, packing the left pixel into the **low** nibble. SSD1322 command table (ugfx `SSD1322.h`,
matching the Solomon datasheet): `15` column (4-px units, 0x00-0x77), `5C` **write RAM (required
before data)**, `75` row, `A0` remap (**2 data bytes**), `A1`, `A2`, `A4`-`A7` display modes,
**`A8` enable partial display**, `A9` exit partial, `AB` function select, `B1`, `B3`, `B4`, `B5`,
`B6`, `B8`, `B9`, **`BB` precharge voltage**, `BE` VCOMH, **`C1` contrast**, `C7` master current,
**`CA` multiplex ratio**, `D1` enhancement B, `FD` lock. So for an SSD1322, ssd130x's `81`, `BC`,
`D5` are undefined, `A8` enables partial mode, `A0` gets one byte instead of two, MUX is never set,
the column range is in the wrong unit, and RAM writes lack `5C`. Doc 04 §1.7's claim stands; it is
now **verified** against both source files. (Recent ssd130x commits up to 2026-10 added SSD1351
and fixed `ssd132x_clear_screen()`, none touch SSD1322.)

### 3.2 Suggested DT corrections for a Track-B board file [I, untested]

```dts
/* watchdog: use the vendor/stock address and the 50 MHz count clock */
wdt_clk: wdt-clk { compatible = "fixed-clock"; #clock-cells = <0>; clock-frequency = <50000000>; };
&twd { status = "disabled"; };                 /* 0xff800400 copy from 4908, likely bogus on 6858 */
watchdog@ff802780 { compatible = "brcm,bcm6345-wdt"; reg = <0x0 0xff802780 0x0 0x8>; clocks = <&wdt_clk>; };

i2c0: i2c@ff85a800 { compatible = "brcm,brcmper-i2c"; reg = <0x0 0xff85a800 0x0 0x58>;
	clock-frequency = <97500>;              /* 375k/390k/187.5k/200k or /4 variants only */
	#address-cells = <1>; #size-cells = <0>; };
i2c1: i2c@ff802100 { compatible = "brcm,brcmper-i2c"; reg = <0x0 0xff802100 0x0 0x58>;
	clock-frequency = <97500>; #address-cells = <1>; #size-cells = <0>; };
/* HSSPI: keep the dtsi node; build CONFIG_SPI_BCM63XX_HSSPI (not SPI_BCMBCA_HSSPI) */
```

---

## 4. OpenWrt and U-Boot

| Project | Status | Evidence |
|---|---|---|
| OpenWrt targets (main) | `airoha apm821xx armsr at91 ath79 bcm27xx bcm47xx bcm4908 bcm53xx bmips d1 econet gemini generic imx ipq40xx ipq806x ixp4xx kirkwood lantiq layerscape loongarch64 malta mediatek microchipsw mpc85xx mvebu mxs octeon omap pistachio qoriq qualcommax qualcommbe ramips realtek rockchip sifiveu siflower starfive stm32 sunxi tegra uml x86 zynq`. **No bcmbca/bcm6858 target** | [V] GitHub API `target/linux` |
| OpenWrt `bcm4908` | BOARDNAME "Broadcom BCM4908 (ARMv8A CPUs Brahma-B53)", **KERNEL_PATCHVER 6.18**, single `generic` subtarget. Devices: ASUS GT-AC5300, Netgear R8000P, TP-Link Archer C2300 v1 built; ASUS GT-AX6000 (BCM4912) and Netgear RAXE500 defined but commented out. No 6858 device, no 6858 DTS built | [V] `target/linux/bcm4908/Makefile`, `image/Makefile` |
| Out-of-tree 6858 OpenWrt / Runner work | None found (GitHub repo search "bcm6858" returns one empty firmware repo; web searches found nothing). lore.kernel.org was not reachable (bot wall), so mailing-list-only patches are [U] | [V]/[U] |
| U-Boot mainline | **`configs/bcm96858_defconfig`**: `ARCH_BCMBCA`, `BCM6858`, `TARGET_BCM96858` (Kconfig "Broadcom 6858 Reference Board", `imply OF_UPSTREAM`), `DEFAULT_DEVICE_TREE="broadcom/bcmbca/bcm96858"` (the Linux DT), TEXT_BASE 0x01000000, COUNTER_FREQUENCY 50 MHz, NAND (`NAND_BRCMNAND_BCMBCA`). No SPI, GPIO, watchdog, Ethernet, USB. `arch/arm/dts/bcm968580xref.dts` (Philippe Reynes 2018, LEDs/HSSPI/NAND) is still built for `CONFIG_BCM6858` but has no defconfig of its own any more | [V] u-boot master `configs/`, `arch/arm/mach-bcmbca/bcm6858/Kconfig`, `arch/arm/dts/Makefile` lines 1041-1043 |
| Relevance | A mainline U-Boot would have to be chain-loaded (`bootm`/`go` from the stock U-Boot) or replace the signed `uboot` volume; it adds nothing the stock U-Boot lacks for RAM-booting today [I] | |

---

## 5. Community knowledge

| Topic | Finding | Evidence |
|---|---|---|
| Firmware versions | pon.wiki maps HH4000 releases to OMCI software versions: 1.7.2 = SGC821011A, 1.7.8.1 = SGC8210140, 1.7.11 = SGC8210154, 2.13 = SGC84000AE, 2.14 = SGC84000DC, 2.14.1 = SGC84000E0, 2.14.2 = SGC8400100. Our image is `scOS SGC821012E`, i.e. a 1.7.x-era build between 1.7.2 and 1.7.8.1 [I]. HH4000 equipment ID `5689`, hardware version `Fast5689Bell`. Firmware 2.13 switched the web/API auth hash from MD5 to SHA-512. Firmware updates come over CWMP (TR-069), not OMCI | [V] [pon.wiki HH4000 masquerade guide](https://pon.wiki/guides/masquerade-as-the-bce-inc-home-hub-4000-with-the-was-110/) |
| WAN facts | Bell Internet VLAN 35, PPPoE with b1 credentials (WAN MTU 1508 / PPPoE 1500 on third-party ONTs); for Aliant/MTS clone the HH4000 MAC. pon.wiki treats the HH4000 as XGS-PON-capable with a GPON variant | [V] same page. Consistent with doc 02 §5 (eth4.35 PPPoE) |
| Bypass options | "PPPoE passthrough" (router on a LAN port does PPPoE itself) and "Advanced DMZ"; users put the 10G RJ45 port into 10GBASE-T SFP+ modules on their routers. No true bridge mode | [V] forum.bell.ca threads (e.g. [PPPoE and ADMZ](https://forum.bell.ca/t5/Internet/Using-PPPoE-and-DMZ-Advanced-DMZ-for-Bridge-Mode-use-of-3rd/m-p/723)) |
| ONT replacement | WAS-110 / X-ONU-SFPP (8311 firmware) masquerading as the HH4000 is the established route for running your own router on Bell fibre | [V] pon.wiki |
| Root / UART / teardown | **Nothing public found** for HH4000 or Giga Hub (no UART pinout, no root method, no chip-marking teardown). The owner's Arduino UART method appears to be original | [V] searches (web, GitHub) returned nothing |
| GPL source | No Sagemcom/Bell GPL drop for the 5689; Sagemcom's opensource portal is gone (LKML 2022 "Sagemcom does not comply with the GPL") | [V] [LKML](https://lkml.iu.edu/hypermail/linux/kernel/2205.3/05155.html) |
| python-sagemcom-api | Python client for the Sagemcom XMO web API; lists 5689 (HH4000, md5, user admin), 5689E/5690 (Giga Hub, sha512) | [V] [GitHub](https://github.com/iMicknl/python-sagemcom-api) |
| pgaskin/lua-sagemcom-openwrt | Lua library that runs **on a separate OpenWrt router** to control a Sagemcom hub (e.g. turn the hub's radios off on WAN up). Uses the XMO JSON-RPC endpoint **`POST /cgi/json-req`** (`req=<json>`), methods `logIn`/`getValue`/`setValue`/`logOut` on XPaths, digest-style auth `hash(user:nonce:hash(pass[:salt]))` then `hash(cred:seq:cnonce:JSON:/cgi/json-req)`, MD5 or SHA-512 chosen from `GUI_ACTIVATE_SHA512ENCODE_OPT`. Tested on the Giga Hub (F@ST 5689E) fw 1.19.6. Created 2024-05, no later commits. **Not** an OpenWrt port of the hub | [V] [README and sagemcom.lua](https://github.com/pgaskin/lua-sagemcom-openwrt) |

---

## 6. Local cross-checks done in this pass

| Check | Result | Evidence |
|---|---|---|
| NAND ID | 0x01/0xdc, S34ML04G2, 512 MiB, OOB 128 (driver uses 16 B/512 B sector, BCH-4) | [V] `work/ubi/fs_filesystem1/data/kernel.log*` |
| RAM | 941052K total, 914000K available; stock DTB memory node says 0x20000000 (512 MiB) | [V] logs, `work/devicetree.dts` |
| SoC | `License chip name: 68580X`; PCIe RC device ID 0x6858; CPU MIDR 0x420f1000. **No die revision (A0/B0) is printed in the logs.** B0 is implied by the 6858_B0 Runner/XRDP_CFE build and the B0 BCM84891 firmware set only [I] | [V] logs |
| PCI | 3 RCs (domains 0-2), each `[14e4:aaa4]` class 0x0280, BAR0 64 KiB, BAR2 8 MiB, BAR4 1 MiB pref; dongle REVINFO `device 0x442a/0x442b, chipnum 0xaaa4, corerev 129` | [V] logs |
| 84891 | `84891 B0 3590:5091 --> 0x1e`, firmware Blackfin B0 v02-02-06 | [V] logs |
| Board IDs | HAL-BSP: Sagem ProductId `SagemcomFast5689_Bell` v1.0 rev 1.0; customer `Bell Internet Box`, `Fast5689Bell` rev 2.2; cmdline `board_type=00030103` | [V] logs |
| Optics | M02099 on i2c-0 0x4e, M02180 on i2c-1 0x4e, both `Part REV 3`; "PON optical interface soldered on board" | [V] logs |
| Zigbee/Z-Wave software | none in rootfs beyond BSP name strings and permanent-param getters | [V] rootfs grep |

---

## 7. Corrections for other docs

| # | Doc / section | Claim | Verdict | Evidence / correction |
|---|---|---|---|---|
| 1 | 00 "Hardware at a glance"; 04 §8; 07 §5 item 1, §8 Q6 | Watchdog at 0xff802780 vs mainline dtsi 0xff800428 ("test both") | **Refined [V/I]** | Vendor `6858_map_part.h`: TIMR 0xff802700, WDTIMR0 0xff802780, WDTIMR1 0xff8027c0. Mainline `twd@ff800400` is copied from BCM4908 and is very likely wrong for 6858. Use 0xff802780 with a 50 MHz clock (driver default is 27 MHz). Q6 can be closed on paper; a hardware test remains [U] |
| 2 | 04 §1.7 item 2 | "Use the mainline `spi-bcmbca-hsspi` (`brcm,bcm6858-hsspi`)" | **Refuted [V]** | 6858 dtsi is `brcm,bcmbca-hsspi-v1.0`, matched by **`spi-bcm63xx-hsspi.c`** (`CONFIG_SPI_BCM63XX_HSSPI`). `spi-bcmbca-hsspi.c` matches only `-v1.1` |
| 3 | 04 §1.7 item 1 | ssd130x SSD1322 path sends SSD1327 commands, not usable | **Confirmed [V]** | Both the source and the SSD1322 command table; also no 0x5C before data, wrong nibble order (§3.1). The incompatibility tag can move from [I] to [V] |
| 4 | 04 §11; 07 §8 Q7; 04 §12 Q6 | I2C: "try i2c-brcmstb brcmper-i2c" [I]; fragment uses `clock-frequency = <100000>` | **Confirmed layout [V], fix the fragment** | Register map identical to `struct bsc_regs`, including CTLHI bit 6. 100000 is not a valid value (driver warns and keeps the current speed); use 97500/93750 or 187500/200000. Also add the second bus @0xff802100 (M02180). Mainline 6858 dtsi has no I2C nodes |
| 5 | 00 "Hardware at a glance" (USB); 04 §0, §6 | "One port (USB3 + USB2) ... it is a USB Type-C receptacle [I]" | **Refuted [V]** | Two receptacles: **USB-C and USB-A (blue, USB3)** side by side (`ext-013.jpg`). Logs only ever show xHCI-SS port 1 and EHCI0 port 1 in use; which connector maps to which controller port is [U]. FUSB301 is for the Type-C one [I] |
| 6 | 04 §6 OpenWrt | `phy-brcm-usb` bcm4908 binding at the same 0x8000c200 [I]; PMB IDs [U] | **Refined [V]** | Address equality confirmed from bcm4908.dtsi and the vendor 6858 header. `bcm-pmb` has no 6858 table; 6858 USB30_2X is PMB bus 1 device 10 (4908: device 17). A `brcm,bcm6858-pmb` entry is needed |
| 7 | 02 §8.3, §9(a) item 4; 07 §5 item 8 | Upstream bcm84881 supports BCM84891 in USXGMII only, no firmware load; extend it | **Confirmed and refined [V]** | Additionally the mainline **PHY ID does not match this part**: board reports 0x35905091 (Broadcom: "84891 B0"); mainline matches 0x3590508x, which Broadcom's own table names "84891L". An ID entry for 0x35905090 is needed. Commit 75171eeff353 is now in torvalds master (plus an LED-framework follow-up, 2026-04) |
| 8 | 03 summary; 07 §8 Q8 | One combined optical assembly or two? | **Answered [V photo]** | One SC/APC connector, one pigtail, one BOSA in one shield (`hh-016/019.jpg`) |
| 9 | 00 "Hardware at a glance" (Voice) | "Si3228x dual FXS daughterboard" | **Refined [V/I]** | Log says `daughter board [SI32282_CB]`, i.e. Si32282. That is Broadcom's voice-board ID; the FCC photos show no separate daughterboard, so the SLIC sits on the main PCB [I] |
| 10 | 00 "Hardware at a glance" | No mention of Zigbee/Z-Wave | **Addition [V]** | FCC VW3FAST5689 certifies 802.15.4 (2405-2480 MHz) and Z-Wave (908/916 MHz) transmitters and the PCB has labelled antennas. No software support in the Bell image; populated state on retail units [U] |
| 11 | 00 "Hardware at a glance" (RAM) | 1 GiB | **Confirmed [V]** | Logs, plus two DRAM packages in the photos (2x 4 Gbit [I]) |
| 12 | 00, 01 (Flash) | S34ML04G2 512 MiB SLC, 2 KiB/128 KiB, BCH-4 | **Confirmed [V]** | Logs |
| 13 | 00 (10G) | BCM84891 B0 on XFI, MDIO 0x1e, firmware loaded at boot | **Confirmed [V]** (ID/addr/fw version from logs); a 5th RJ45 with separate magnetics in the photos |
| 14 | 00, 06 (WiFi) | 3x BCM43684 B0, one per PCIe RC; 1x 2.4 + 2x 5 GHz; no 6 GHz | **Confirmed [V]** | PCI IDs in logs; FCC grant has no 6 GHz for VW3FAST5689 (6 GHz is only on the Giga Hub 5689E); antenna labels 2.4G x3 + 2.4/5G + 5G x6 + ZB/5G x2 |
| 15 | 06 §(mainline) | brcmfmac does not support BCM43684 [I high-confidence] | **Confirmed [V]** | No 43684/0xaaa4 in `brcm_hw_ids.h` or `pcie.c` device/firmware tables |
| 16 | 01 §12.4; 02 §8.3 | Mainline `bcm6858.dtsi` has "CPUs, GIC, timer, UART, a few peripherals"; 02: "(CPU, UART, timers)" | **Refined [V]** | Current dtsi also has GPIO x8, HSSPI, NAND, RNG, LED controller, UART1, PL081 DMA and the TWD/watchdog node (likely at the wrong address). It has **no** I2C, USB, PCIe, PMB, pinctrl, MDIO or Ethernet |
| 17 | 02 §8.3 | "U-Boot mainline has BCM6858 (bcm968580xref) support: bare-bones SoC and UART" | **Refined [V]** | The live target is `bcm96858_defconfig` / `TARGET_BCM96858` (Broadcom 2022, OF_UPSTREAM, UART + brcmnand-bcmbca). The old `bcm968580xref.dts` is still built but has no defconfig |
| 18 | 01 §12.4; 02 §8.3; 09 §0 | OpenWrt has no bcmbca/6858 target; `bcm4908` is BCM4908-only | **Confirmed, refined [V]** | `bcm4908` target (kernel 6.18) covers BCM4906/4908 boards and defines a BCM4912 board (disabled). No 6858 device |
| 19 | 04 §4.3 | No mainline 6858 pinctrl; `pinctrl-bcm4908` uses the same TEST_PORT/LOAD_MUX scheme | **Confirmed [V]** | 4908: base 0x560, DATA_MSB +4, DATA_LSB +8, CMD +0xc, LOAD_MUX 0x21. 6858 stock regs 0x554/558/55c = base 0x550 with the same offsets |
| 20 | 04 §4.2 | Mainline gpio banks `brcm,bcm6345-gpio` at 0x500+4n / 0x520+4n | **Confirmed [V]** | dtsi gpio0-7 |
| 21 | 04 §5 (LED controller) | Mainline is `brcm,bcm63138-leds` (`leds-bcm63138.c`) | **Confirmed [V]** | The 6858 dtsi already has `led-controller@800` (reg size 0xdc vs stock 0xe8) |
| 22 | 01 §12.1; 05 | Firmware is 4KOS 20.2.152, scOS SGC821012E | **Context added [I]** | pon.wiki's version table places SGC821012E between Bell 1.7.2 (SGC821011A) and 1.7.8.1 (SGC8210140). Current Bell releases are 2.14.x (SGC8400xxx) |
| 23 | 00 (Screen) | SSD1322 167x42 | **Confirmed [V]** | Log `SSD1322 screen: width=167 (upscaled to 168) height:42`; panel visible in photos |
| 24 | 00 (Sensors / MCU / FUSB301) | 2x TMP102 @0x48/0x49, MCU @0x78, FUSB301 @0x25 | **Confirmed by logs/DT [V]**; chip markings not visible in FCC photos [U] |
| 25 | 00 "Bottom line" item 5 | "buttons/LEDs (gpio-keys / gpio-leds on brcm,bcm6345-gpio), ... watchdog ... have documented paths" | **Refined** | GPIO/LEDs/buttons/RNG/NAND/UART/HSSPI work with existing drivers [V]; I2C and watchdog need DT fixes (#1, #4) [V/I]; USB needs a PMB table (#6); 10G PHY needs a new ID and host modes (#7) |
| 26 | 07 §8 Q4 | Front-panel port order | **Partly answered [V]** | Rear-panel silkscreen is LAN 1..LAN 4 left to right next to TEL 1/2, then the 10G jack. Mapping to eth0-3 is still [U] |
| 27 | (UART) 00 / 01 | UART console at 115200 | **Not resolved from photos [U]** | No legible header silkscreen in FCC photos; no public pinout found |

---

## 8. Sources

- FCC: [fccid.io/VW3](https://fccid.io/VW3), [fccid.io/VW3FAST5689](https://fccid.io/VW3FAST5689) (exhibits 5053517 internal photos, 5053516 external, 5053515 label, 5053430 Zigbee DTS report, 5053487 Z-Wave report), [fccid.io/VW3FAST5689E](https://fccid.io/VW3FAST5689E) (5736498, 6032001 internal photos). fcc.report and apps.fcc.gov returned 403 from this host.
- Linux master: `arch/arm64/boot/dts/broadcom/bcmbca/{bcm6858.dtsi,bcm96858.dts,bcm4908.dtsi,Makefile}`, `drivers/watchdog/bcm7038_wdt.c`, `drivers/i2c/busses/i2c-brcmstb.c`, `drivers/spi/spi-bcm63xx-hsspi.c`, `drivers/spi/spi-bcmbca-hsspi.c`, `drivers/mtd/nand/raw/brcmnand/bcmbca_nand.c`, `drivers/phy/broadcom/phy-brcm-usb*.c`, `drivers/pmdomain/bcm/bcm-pmb.c`, `drivers/pci/controller/pcie-brcmstb.c`, `drivers/pinctrl/bcm/pinctrl-bcm4908.c`, `drivers/net/phy/bcm84881.c`, `drivers/gpu/drm/solomon/ssd130x.c`, `drivers/net/wireless/broadcom/brcm80211/{include/brcm_hw_ids.h,brcmfmac/pcie.c}`, `drivers/tty/serial/bcm63xx_uart.c`, `drivers/gpio/gpio-mmio.c`, `drivers/char/hw_random/iproc-rng200.c`, `drivers/hwmon/tmp102.c`, `drivers/net/ethernet/broadcom/` (listing) - all at [github.com/torvalds/linux](https://github.com/torvalds/linux).
- BCM84891 commit: [75171eeff353](https://github.com/torvalds/linux/commit/75171eeff3538c6859b50f6a516342c8f1ed6e00).
- SSD1322 command codes: [ugfx SSD1322.h](https://git.ugfx.io/uGFX/ugfx/src/branch/master/drivers/gdisp/SSD1322/SSD1322.h); datasheet mirror [ssd1306.readthedocs.io SSD1322.pdf](https://ssd1306.readthedocs.io/en/stable/_downloads/SSD1322.pdf).
- U-Boot master: `configs/bcm96858_defconfig`, `arch/arm/mach-bcmbca/bcm6858/Kconfig`, `arch/arm/dts/bcm968580xref.dts`, `arch/arm/dts/Makefile` at [github.com/u-boot/u-boot](https://github.com/u-boot/u-boot).
- OpenWrt main: `target/linux/bcm4908/{Makefile,image/Makefile}`, `target/linux/` listing at [github.com/openwrt/openwrt](https://github.com/openwrt/openwrt).
- Community: [pon.wiki HH4000 guide](https://pon.wiki/guides/masquerade-as-the-bce-inc-home-hub-4000-with-the-was-110/), [python-sagemcom-api](https://github.com/iMicknl/python-sagemcom-api), [pgaskin/lua-sagemcom-openwrt](https://github.com/pgaskin/lua-sagemcom-openwrt), [forum.bell.ca PPPoE/ADMZ thread](https://forum.bell.ca/t5/Internet/Using-PPPoE-and-DMZ-Advanced-DMZ-for-Bridge-Mode-use-of-3rd/m-p/723), [LKML 2022 Sagemcom GPL](https://lkml.iu.edu/hypermail/linux/kernel/2205.3/05155.html).
- Local: `work/eth/ref/merlin/release/src-rt-5.02L.07p2axhnd/shared/opensource/include/bcm963xx/6858_map_part.h`, `.../include/pmc/pmc_drv.h`, `.../drv/phys/phy_drv_ext3.c` (ASUS-Merlin GPL tree), `work/devicetree.dts`, `work/os/vmlinux.elf` (`bcm63000_xfer`), `work/ubi/fs_filesystem1/data/kernel.log*`, `work/periph/bsp_bell_gpio.txt`, `work/rootfs`.
