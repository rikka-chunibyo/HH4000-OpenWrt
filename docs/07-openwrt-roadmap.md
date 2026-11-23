# 07 - OpenWrt Porting Roadmap (consolidated)

This page combines the feasibility findings from docs 01–11 into one plan (updated after the
pass-2 verification on 2026-11-04). Evidence for each claim
is in the referenced section. Items marked **[I]** are inferred, and **[U]** means untested on
hardware.

---

## 1. The core constraint

On BCM6858 **every Ethernet port, the PON and WiFi offload go through the XRDP Runner**, and the
Runner driver (`rdpa.ko`) is proprietary and built for exactly `4.1.52-5.02L.07` (02 §2, §9). That
gives two different kinds of port:

| | Track A: **Hybrid** (stock BCM kernel + OpenWrt userspace) | Track B: **Native** (modern kernel, OpenWrt target) |
|---|---|---|
| Kernel | Stock 4.1.52-5.02L.07 binary. A full rebuild from the GPL tree is **not possible** (6858 board/PCIe/PMC code ships only as prebuilt objects), but **ABI-compatible extra modules can be built** (05) | Current OpenWrt kernel + `bcm6858.dtsi` |
| Userspace | **OpenWrt 24.10 with fw3/iptables** (25.12 probably). Needs `xt_comment.ko` (built). No nftables/fw4, no VLAN-aware bridge (05) | Any current OpenWrt |
| LAN 4x1G | Stock `bcm_enet` + `rdpa` | **New drivers needed**: LPORT MDIO, QEGPHY, XLMAC/LPORT, minimal XRDP datapath |
| 10G eth4 | Works, with BCM84891 firmware loaded by the stock kernel | BCM84891 XFI + MDIO firmware load + Merlin16 SerDes PCS (port GPL code) |
| PON | Possible with stock pondrv/gponstack/ngponstack/bcmgpon + `omcid` (hard, fragile) | Long-term: open `hh-pon` driver (PLOAM core done, 15) on top of the Runner driver with stock Runner images + open OMCI daemon (12–15) |
| WiFi (3x BCM43684) | Stock `dhd.ko` + dongle firmware + NVRAM/calib | **Possible**: matching DHD source in the Merlin 5.04 tree, add-ons optional, stock `rtecdc.bin` reusable. Needs a 6858 PCIe host driver first (06). brcmfmac has no support |
| HW NAT / flow offload | Yes (pktflow/pktrunner/fcctl) | No (CPU path only; [I] well below 10G line rate) |
| Firewall | iptables/fw3 only (no nf_tables, no namespaces in the stock config) | Full modern OpenWrt (fw4, procd jails) |
| Screen, buttons, LEDs, MCU, sensors, USB | Stock drivers or userspace | Mainline/small drivers (04 §11) |
| Maintenance | Frozen 2018-era kernel | Upstreamable, but months of driver work |

**Recommendation:** do Track A first to get a usable OpenWrt-like router with full hardware. Run
Track B as a long-term project, starting with MDIO and one LAN port.

---

## 2. Phase 0: Prerequisites and safety

1. Keep `raw/backup/` (mtd0–mtd6 + SHA-256) as the golden set. Also keep an Arduino/UART bridge
   connected during any flash operation (01 §12.3).
2. Confirm the following over UART (open questions 01 §13):
   - Reset banner: confirm no "Device in Secure Mode" message, i.e. the SoC is not fused.
   - Run `printenv` and save the full default env.
   - Run `run reset_env; saveenv`, power-cycle, then `printenv`. This proves that a persistent env
     works via the new `bootenv` UBI volume (01 §6.3).
   - Run `tftp` against a PC at `serverip` (default 192.168.1.10) to learn which physical port
     U-Boot's `eth0` uses.
3. Decide on the WAN: eth4 Ethernet WAN behind an external ONT (03 §6), or stock-PON hybrid.

## 3. Phase 1: RAM-boot experiments (non-destructive)

At the U-Boot `=>` prompt. **Exact, pass-2 verified commands are in 01 §12.1**; in summary:
U-Boot has `booti` (arm64 Image) and legacy-uImage `bootm`, **no FIT**; stage images around
0x10000000 (kernel) / 0x0f000000 (DTB); kernel decompression limit is 32 MiB. TFTP uses the Runner
bootloader driver on whichever port links first (defaults 192.168.1.1 → server 192.168.1.10).
```
tftp 0x10000000 Image; tftp 0x0f000000 board.dtb
setenv bootargs "console=ttyS0,115200 coherent_pool=4M cpuidle_sysfs_switch pci=pcie_bus_safe rootwait cma=0M ..."
booti 0x10000000 - 0x0f000000
```
**Watchdog:** U-Boot starts the SoC watchdog (~60 s [I]) before handing over. The kernel must run
`bcm7038_wdt` (0xff802780, 50 MHz) or keep it fed, or the board resets about a minute after boot (04 §8).
- Track A: use the **stock kernel uImage + stock DTB** with an OpenWrt initramfs (32-bit armhf or
  aarch64 userspace; the stock kernel runs both, since stock userspace is 32-bit) [I].
- Track B: an OpenWrt arm64 initramfs on a new kernel with a board DTS built from
  `bcm6858.dtsi` and the fragment in 04 §11. UART, NAND, GPIO, LEDs, buttons, I²C, TMP102 and the
  watchdog should come up first; there is no networking until the Track B drivers exist.

Note: an alternative for Track A is to skip replacing the root at all and run OpenWrt userspace in a
chroot on top of the stock rootfs. This needs only the existing `~f` style hook (05 §6).

## 4. Phase 2: Track A (hybrid) bring-up

Hardware init that must be replayed, in stock order (05 §2, 02 §9(b), 03 §7):

1. **License blob → `/proc/driver/license`** (from the `calib` volume, `bcm_license.txt`). Without it the
   accelerator modules refuse to run (05).
2. `insmod` (Ethernet-only minimum): `bcmlibs bdmf opticaldet bcm_pondrv rdpa_gpl rdpa_gpl_ext rdpa
   rdpa_usr rdpa_mw pktflow bcm_enet [cmdlist pktrunner] bcmvlan rdpa_cmd`, then `bdmf_shell -c init`.
   `rdpa` hard-depends on `bcm_pondrv` even with Ethernet WAN (02 §2). [I] Whether `wlcsm`,
   `chipinfo` or `bcm_license` are also required is untested.
3. **WAN selection** through the Broadcom scratchpad/PSP: `RdpaWanType=GBE`, `WanOEMac=EMAC4` (eth4).
   This is read once at boot (02 §5, 03 §2). Then run `wanconf` or the equivalent `bcmsw` ioctl. For Bell
   Ethernet WAN: `eth4.35` PPPoE, MRU 1492; IPTV on VLAN 36 (02 §5).
4. Optional offload: `fcctl`/`fc` + `rdpaconf` steps; note the stock WAN frame cap of 1536 bytes (02 §4).
5. **WiFi:** `wfd hnd emf igs wlcsm cfg80211 dhd` + firmware `etc/wlan/dhd/43684b0/rtecdc.bin` +
   NVRAM `calib/kernel_nvram.setting`. The stock Broadcom-patched hostapd 2.9 or `wl` tooling is
   needed; OpenWrt's stock hostapd/netifd WiFi scripts will not drive dhd as-is (06 §2–3).
6. **Peripherals:** `hard_watchdog` (or kick the SoC WDT; **must** be handled or the box reboots
   after ~85 s if it was armed), `ssd1322` (fb0), `bsp_gpio_*`/`drv_gpio` (buttons/LEDs) or raw GPIO
   via `/dev/mem`, `mcu-*`, `fusb301` (or two `i2cset` writes), `tmp102` (04).
7. Userspace: 32-bit glibc Broadcom tools (`ethctl`, `bdmf_shell`, `wanconf`, `vlanctl`, `fcctl`,
   `tmctl`) need a glibc compat area if OpenWrt uses musl, or reimplement the few ioctls (02 §9(b)).
8. Kernel limits: no nf_tables, no namespaces, no phylib/DSA. **OpenWrt 24.10 userspace with fw3/iptables
   works** (procd/netifd/ubus tolerate the missing features; 05 pass 2). Use VLAN subinterfaces (no
   VLAN-aware bridge) and treat eth0–eth4 as plain netdevs bridged in software or via `bcmvlan`.
9. Extra kernel modules: build them out-of-tree from the Merlin 4.1 tree with `LOCALVERSION=-5.02L.07`.
   Struct layouts match the stock kernel, except that conntrack users need the small patch in
   `work/os/pass2/`. `xt_comment.ko` (required by fw3) and WireGuard are already built (05).

**Getting the GPL source** from Bell/Sagemcom is still worth requesting: the public tree lacks the 6858
board/PCIe/PMC sources, so a full kernel rebuild currently isn't possible.

## 5. Phase 3: Track B (native) driver work, in order

From 02 §9(a), 04 §0/§11:

| # | Item | Reference source | Difficulty |
|---|---|---|---|
| 1 | Board DTS on mainline `bcm6858.dtsi`: UART, NAND, `brcm,bcm6345-gpio` banks, HSSPI (`spi-bcm63xx-hsspi`), RNG, LED controller (`leds-bcm63138`, sw mode only) already exist. Add: gpio-keys-polled, gpio-leds, I²C x2 (`brcm,brcmper-i2c`, `clock-frequency = <97500>`), TMP102, **watchdog fix** (0xff802780, 50 MHz; mainline address is wrong) | 04 §4, §8, §11; 10 | S |
| 2 | Pinctrl: add a 6858 pin table to `pinctrl-bcm4908` (same TEST_PORT/LOAD_MUX scheme at base 0xff800550) | 04 §4.3; 10 #19 | S–M |
| 3 | SSD1322 SPI driver (draft DRM driver + userspace test tool in `work/periph/`; mainline ssd130x can't drive it) | 04 §1 | M |
| 4 | MCU (I²C 0x78), FUSB301 setup, USB PHY (`phy-brcm-usb` bcm4908 sequence + 6858 IRQs) + a **6858 table in `bcm-pmb`** for power domains | 04 §3, §6; 10 #6 | S–M |
| 5 | LPORT MDIO bus (C22/C45) | 02 §3 | S |
| 6 | QEGPHY power-up + PHY driver | 02 (GPL `phy_drv_6858_egphy.c`) | M |
| 7 | XLMAC + LPORT + Merlin16 SerDes as a phylink MAC/PCS | GPL `lport_drv.c`, `serdes_access.c`, `merlin_direct_access.c` | M–H |
| 8 | BCM84891: add ID 0x35905090, XFI mode, MDIO firmware load, reset GPIO80 (extend upstream bcm84881) | GPL `blackfin_b0_firmware.h`; 02, 10 #7 | M |
| 9 | XRDP Runner datapath: **spec in doc 11** (field layouts in `work/regdb/rdpa_fields.*`, init recipe for eth0 + eth4, rings/descriptors, IRQs) | 11; GPL XRDP_CFE project | **H** (first usable router ≈ 3–5 months) |
| 10 | 6858 PCIe host driver (prerequisite for WiFi) | stock `bcm963xx-pcie`; GPL tree | M–H |
| 11 | DHD WiFi port to a modern kernel with stock `rtecdc.bin` | Merlin 5.04 DHD source (06) | M–H |

First Track B networking milestone: "MDIO + QEGPHY link + XRDP_CFE-style CPU datapath on eth0".

## 6. Permanent installation (either track)

From 01 §12.2:
1. Keep `cfe`, `secondaryboot`, `uboot`, `uboot-rescue`, `nvram`, `permanent_param`, `calib` and
   the stock `rescue` bank **untouched**.
2. Stage OpenWrt on a **new UBI device on `conf` (mtd4)**. Nothing in the stock firmware references
   mtd4/mtd5 (01, pass 2). **Don't use the `ubi` partition (mtd3)**: a stock init script auto-creates
   volumes there.
3. Create the env volume (`run reset_env`), then set
   `bootcmd='run owrt_boot; sbp'` (with `owrt_boot` doing `ubi part conf; ubi read ...; booti ...`), and
   `saveenv`. If OpenWrt fails to load, `sbp` boots stock as before. Exact three-step procedure with
   reboot tests between steps: 01 §12.2. U-Boot has no FIT support, so ship a raw Image + DTB.
4. **Do not** write an unsigned image into `operational` while `bootcmd=sbp`; it will be rejected
   (01 §7).
5. Recovery: remove `bootenv` to return to the default `bootcmd=sbp`, use `force-rescue`, or use
   UART + U-Boot `ubi write` from TFTP. Last resort is a NAND reprogram from `raw/backup`.

## 7. WAN options summary (03 §6)

| Option | Effort | Notes |
|---|---|---|
| eth4 (10GBASE-T RJ45) → external ONT / media converter | Low | Works with either track. eth4 is **RJ45, not SFP+**; an SFP+ ONT stick needs a 10G SFP+↔RJ45 converter or switch. This line was **GPON** on a Huawei OLT, so the ONT must match the line technology |
| Stock firmware kept as ONT/bridge feeding an OpenWrt box | Lowest | PPPoE passthrough exists in the stock config but is disabled |
| On-board PON in the Track A hybrid | High, fragile | Needs the stock PON modules, `omcid` and replacement of Sagemcom HAL glue (`/var/omcid/omcid.conf` generation, supervision). Recipe in 03 §7 |
| On-board PON in Track B | Long-term, specified | GPON fully specified in **12–14**; design, milestones M0–M6 and the `src/hh-pon` skeleton in **15**. Needs the Track B Runner driver with the stock `fw_binary_*` images (PLOAMs arrive on Runner ring 6), then OMCI. First transmit tests against a lab OLT |
| Active Ethernet over the on-board optics | Unknown, cheap to test | 08 §5: firmware has AE 2.5G/5G/10G WAN modes. Test on stock firmware with an opposite-wavelength BiDi optic |

## 8. Consolidated open questions (need the device)

1. SoC fuse state and the full U-Boot `printenv`/version banner (01 §13).
2. Whether `saveenv` persists after `run reset_env` (01 §6.3).
3. ~~Which scratchpad copy is live~~ **Resolved (pass 2):** the kernel reads `/opt/conf/scratchpad`
   (filesystem1 copy, GBE); nothing mounts the overlay copy (01, 02).
4. Jack-to-eth mapping and LED colours need a cable test (HAL tables say PHYn = eth(n−1); rear jacks
   LAN1–4 + "10G"). BCM84891 reset is GPIO80 "Phy rst all", active-low [V from tables]; timing untested.
5. Button mapping: **resolved on paper (~90%)**: UP 13, DOWN 14, OK 15 per the shipped firmware's
   whole button path (04). Confirm with `gpioget`.
6. Watchdog: **resolved on paper**: 0xff802780 per Broadcom's 6858 header; mainline's address is wrong.
   Also confirm U-Boot's pre-boot watchdog timeout over UART.
7. I²C: **resolved on paper**: register layout identical to `i2c-brcmstb`; use 97500 Hz.
8. ~~One or two optical assemblies~~ **Resolved:** one BOSA (FCC photos, 10).
9. Runtime OMCI configuration values on a live stock unit (03).
10. Whether the earliest bootloader stage handles a new `bootenv` volume correctly; test step A of the
    install alone first (01 §12.2).
11. Whether stock modules load with ABI-compatible rebuilt companions (e.g. `xt_comment`) on the device (05).
