# Bell Home Hub 4000 (Sagemcom F@ST 5689): Reverse-Engineering Overview

This is the index and summary of the HH4000 hardware and firmware analysis, written for an OpenWrt
port. Every detailed claim in the numbered docs is tagged **[V]** (verified, with a file, offset,
symbol or log line) or **[I]** (inferred). This page only summarises; follow the links for evidence.

Firmware analysed: `4KOS_20.2.152_prod` ("scOS SGC821012E", built 2022-04-15). The `rescue` bank holds
the older `4KOS_20.2.143.3_prod`.

## Documents

See the [research index](README.md).

## Hardware at a glance

| Item | Finding | Doc |
|---|---|---|
| SoC | Broadcom **BCM6858** (68580X), 4x Cortex-A53 (Brahma-B53), ARMv8 | 01, 05 |
| RAM | **1 GiB** (2 DRAM packages; kernel reports 941052K). The stock DTB says 512 MB; U-Boot rewrites `/memory` from the memory controller on every boot | 01, 10 |
| Flash | 512 MiB SLC NAND, Spansion S34ML04G2, 2 KiB page, 128 KiB block, BCH-4 | 01 |
| Ethernet LAN | eth0–eth3: internal QEGPHY, MDIO 1–4, clause 22, 1G. Rear jacks LAN1–LAN4 (eth mapping per HAL tables: PHYn = eth(n−1)) | 02 |
| Ethernet 10G | eth4: separate "10G" jack, external **BCM84891 B0** 10GBASE-T PHY (ID 0x35905091, *not* matched by mainline `bcm84881`), XFI, MDIO 0x1e, clause 45, reset GPIO80. Firmware loaded over MDIO every boot | 02, 10 |
| Datapath | Broadcom **XRDP Runner**. Every port's RX/TX goes through Runner CPU rings; register map + fields recovered, open-driver spec in doc 11 | 02, 11 |
| PON | **One SC/APC connector, one BOSA**, on-board (no SFP cage). M02099 GPON path and M02180 XGS-PON path, GPIO-selected, one WAN SerDes. This unit ran GPON on Bell, later Ethernet WAN on eth4 (live setting: GBE) | 03, 10 |
| WiFi | 3x **BCM43684 B0** FullMAC (4x4 11ax), one per PCIe RC: wl0 2.4 GHz, wl1/wl2 5 GHz (upper/lower). No 6 GHz on this model (the Giga Hub 5689E is the same board plus 6 GHz) | 06, 10 |
| Other radios | FCC filing certifies **Zigbee (802.15.4) and Z-Wave** radios with PCB antennas; unused by Bell firmware, fitted state on retail units unknown | 10 |
| Screen | SSD1322, 167x42 4-bit grey, HSSPI CS5 (GPIO54), D/C GPIO25, RST GPIO75, PWR GPIO74. U-Boot initialises it | 04 |
| Buttons | UP = GPIO13, **DOWN = GPIO14, OK = GPIO15** (stock board table; the DTS names are swapped), rear RESET = GPIO59, active-low, polled. The MCU owns a separate power/reboot button | 04 |
| MCU | Silicon Labs EFM8BB10F8G at I²C-0 0x78: soft-power/reboot supervisor, a green LED, thermal latch. Needs no servicing | 04 |
| USB | **Two receptacles: USB-C and USB-A (USB3)**. FUSB301 Type-C controller (source-only 1.5 A). Logs only show one xHCI SS + one EHCI port in use | 04, 10 |
| Buzzer/speaker | **None found** (no GPIO, PWM, MCU command or strings) | 04 |
| Sensors | 2x TMP102 (0x48/0x49 on I²C-0). No fan | 04 |
| Voice | Si32282 dual FXS on the main board (proprietary SLIC stack) | 04, 10 |
| Watchdog | SoC WDT at 0xff802780 (50 MHz). **U-Boot arms it (~60 s) before Linux**; stock `hard_watchdog.ko` then runs it at 85 s. Mainline dtsi has it at the wrong address | 04, 10 |

## Software at a glance

- **Kernel:** Linux 4.1.52-5.02L.07 (Broadcom BCA SDK 5.02L.07), gcc 5.5, aarch64. vermagic
  `4.1.52-5.02L.07 SMP preempt mod_unload aarch64`. No modversions, no module signing. IKCONFIG and
  kallsyms are present. Symbolised ELFs are at `work/os/vmlinux.elf` and `work/eth/vmlinux.elf`, the
  config at `work/os/kernel.config`.
- **Userspace:** 32-bit ARMv7 (soft-float), glibc 2.27, BusyBox 1.30.1. It is an OpenWrt-derived build (preinit,
  rc.d) **without** procd/ubus/uci. Sagemcom hg6d/XMO middleware, Plume/OpenSync, TR-069.
- **Boot:** BootROM → `cfe` (CFE-ROM 3.24.0) → `secondaryboot` (CFE) → `uboot` (Sagemcom U-Boot,
  `bootcmd=sbp`) → `operational` GSDF, with `rescue` as the fallback bank.
- **Secure boot:** U-Boot's `sbp` checks the RSA-2048 Sagemcom GSDF signature on the normal path.
  The images on flash are plaintext and carry only Sagemcom signatures, which points to the SoC
  **not** being BTRM-fused [I]. `bootm` of a RAM image runs no signature check (proven by the
  Arduino method). The U-Boot env lives in a UBI volume `bootenv` that **does not exist yet** on this
  unit. `run reset_env` creates it, after which a persistent `bootcmd` becomes possible (01 §6.3).

## Bottom line for the OpenWrt goal

1. **Booting OpenWrt is easy.** Stock U-Boot will `booti` an arm64 Image or `bootm` a legacy uImage
   from RAM (TFTP or UBI); it has **no FIT support**. The env can be made persistent with a new
   `bootenv` volume, and a persistent install fits on a new UBI device on the empty `conf`
   partition with stock as automatic fallback (01 §12). Remember the watchdog U-Boot arms.
2. **Two OpenWrt tracks are now well defined:**
   - *Hybrid:* **OpenWrt 24.10 userspace with fw3/iptables on the stock 4.1 kernel** and stock
     modules. Extra modules (e.g. `xt_comment`, WireGuard) can be built ABI-compatibly from the GPL
     tree (05). Everything works, including Runner offload, WiFi and (with work) PON.
   - *Native:* modern kernel + new drivers. Ethernet needs the XRDP Runner driver specified in doc 11
     (first usable router ≈ 3–5 months). Most small peripherals already have mainline drivers (10).
3. **WiFi on a modern kernel is possible.** Matching Broadcom DHD source exists in the Merlin 5.04
   tree, its Broadcom add-ons are optional, and the stock dongle firmware can be reused. It needs a
   6858 PCIe host driver first (06).
4. **On-board PON is now fully specified for GPON, but it is a long project.**
   - What is specified: the SerDes sequence (no microcode needed for GPON), the MAC init and
     every PLOAM handler (13), the PLOAM/OMCI channels through the Runner (14) and the full stock
     OMCI MIB (12).
   - What is built: an open PLOAM codec and O1–O7 state machine, host-tested, and a kernel module
     skeleton that compiles for arm64 (`src/hh-pon`, 15).
   - The catch: PLOAMs arrive through the Runner, so native PON needs the full Runner driver and the
     stock Runner firmware images, loaded from the owner's own flash (14, 15).
   - Short term: the hybrid stock kernel already runs PON, or put **eth4 behind an external ONT**.
5. **Peripherals are tractable.** GPIO/LEDs/buttons/NAND/UART/HSSPI/RNG work with existing
   mainline drivers; I²C (`i2c-brcmstb`), watchdog and USB need DT fixes or a PMB table; the screen
   needs a small SPI driver (a userspace test tool and a draft DRM driver are in `work/periph/`).

See [07-openwrt-roadmap.md](07-openwrt-roadmap.md) for the phased plan.

