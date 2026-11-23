# 06 - WiFi (HH4000 / FAST5689 / BCM6858)

Scope: radios, chips, PCIe mapping, dongle firmware, NVRAM/calibration, country
code, driver stack (dhd/wfd/cfg80211/hostapd), mesh/Plume, OpenWrt feasibility,
and porting DHD to a modern kernel. Legend: **[V]** Verified (path/log/source cited) ·
**[I]** Inferred · **[U]** Unverified / unknown. `$P` = `/path/to/HH4000-OpenWRT`.

**Pass-2 summary:**
- **Three BCM43684 B0** FullMAC dongles, one per PCIe RC: **wl0 = 2.4 GHz**, and **wl1/wl2 = 5 GHz** split into upper and lower sub-bands, all 4x4. There is no 6 GHz radio on the HH4000 [V].
- Mainline brcmfmac and linux-firmware have **no BCM43684 support** (checked 2026-11-04) [V]. The stock firmware also speaks Broadcom's BCA PCIe IPC rev **0x82**, which brcmfmac's shared-memory protocol (rev 5–7) cannot handle [V code, I outcome].
- **WiFi works today only on the stock 4.1 kernel with the stock modules.** On a modern kernel, the route is to **port Broadcom's DHD from source**. The ASUS-Merlin 5.04 tree has DHD **17.10.188.6401**, the same 17.10 branch as the stock firmware **17.10.121.41**, and its IPC check accepts the older dongle revision. Every BCA-specific dependency (wlcsm, hnd, emf/igs, wfd, Runner offload, NBUFF/BLOG) is behind build flags and can be turned off. The work that remains is a cfg80211 API port from about 5.10 up to 6.x, a PCIe RC driver for 6858 on the new kernel, and calibration NVRAM handling. Estimate: **weeks to a few months for an experienced kernel developer** [I] (§4).

---

## 1. Radios & chips

### 1a. PCIe mapping [V] (kernel.log, devicetree.dts)
Three `bcm963xx-pcie` GEN2 single-lane root complexes, one dongle each. *(Pass 2 correction: the dongle is at `000N:01:00.0` behind the RC's root port at `000N:00:00.0`.)*

| Radio | PCIe RC (DT) | Root port | Dongle | Class | DT reserved-mem |
|---|---|---|---|---|---|
| wl0 | `/pcie@80040000` | `0000:00:00.0 [14e4:6858]` type 01 | `0000:01:00.0 [14e4:aaa4]` | 0x028000 | dhd0 0xb00000 |
| wl1 | `/pcie@80050000` | `0001:00:00.0 [14e4:6858]` | `0001:01:00.0 [14e4:aaa4]` | 0x028000 | dhd1 0xb00000 |
| wl2 | `/pcie@80060000` | `0002:00:00.0 [14e4:6858]` | `0002:01:00.0 [14e4:aaa4]` | 0x028000 | dhd2 0xb00000 |

- `0xaaa4` is decimal **43684**: the PCI device ID is the chip number. Broadcom's own header confirms it: `#define BCM43684_CHIP_ID 0xaaa4 /* 43684 chipcommon chipid */` (`…/impl87/main/src/include/bcmdevs.h:570`; the same line is in impl69) [V]. Class 0x0280 = "network controller, other".
- The link trains GEN2 x1 and ASPM is disabled (`Disabling PCIe ASPM for vendor 14e4 device aaa4`).
- `dhd_sync_with_dongle: … chipnum 0xaaa4, corerev 129` [V].
- The dhd0–2 reserved regions (11 MiB each) are the **Runner DHD-offload flow-ring pools** (`dhd_runner_flowmgr_init: dhd0_rnr bootmem … size<11534336>`) [V]. They are only needed while Runner offload is enabled (§2).

### 1b. Chip identity and band plan [V]
Per-radio NVRAM (`$P/work/ubi/calib/kernel_nvram.setting`) and runtime `GET_REVINFO`:

| Interface | NVRAM idx | devid (log + NVRAM) | Broadcom meaning (`bcmdevs.h`) | XMO radio (`conf/backup-cfg-WiFi-31.xml`) | Sub-band | Default channel |
|---|---|---|---|---|---|---|
| **wl0** | 1 | `0x442a` | `BCM43684_D11AX2G_ID` "43684 802.11ax 2G device" | `RADIO2G4`, `OperatingFrequencyBand=2_4GHZ`, b/g/n/ax | – | auto, 20 MHz |
| **wl1** | 2 | `0x442b` | `BCM43684_D11AX5G_ID` "43684 802.11ax 5G device" | `RADIO5G2`, `SplittedOperatingFrequencyBand=UPPER`, a/n/ac/ax | **5 GHz high** | 149, auto bandwidth |
| **wl2** | 3 | `0x442b` | `BCM43684_D11AX5G_ID` | `RADIO5G`, `SplittedOperatingFrequencyBand=LOWER`, a/n/ac/ax | **5 GHz low** | 44, 80 MHz |

- The band per radio is **[V]**: the devid defines, plus XMO `LOWER`/`UPPER`.
- Which 5 GHz radio is wl1 and which is wl2 is **[I], strong**. `etc/checkwifi.sh` checks `AccessPoint[@uid='5']` on **wl1** and `AccessPoint[@uid='3']` on **wl2**. In the XMO config, AP 5 = `IPTV_5G` → SSID `WL_IPTV_5G` → `RADIO5G2` (UPPER), and AP 3 = `VID2` → `WL_DATA_5G` → `RADIO5G` (LOWER). The main 5 GHz data SSID is on wl2 (lower band). The upper-band radio carries the wireless-IPTV SSID plus `WL_DATA_5G2`.
- Both 5 GHz radios have `DFSChannel=NO_SUPPORT` in this config (the 2.4 GHz radio says `FULL_SUPPORT`). The defaults therefore sit in U-NII-1 (ch 44) and U-NII-3 (ch 149), although the FCC grant also certifies U-NII-2A/2C (doc 10 §1) [V config].
- `RegulatoryDomain=CAI`, `RegulatoryRegionSubRegion=CA/0`.
- All three radios have `sromrev=18`, `venid=0x14e4`, `phycal_tempdelta=40`, `subband5gver=0x5`, `txchain=rxchain=0xf` → **4x4:4**. `aa2g=0xf` appears only on radio 1; radio 1 also carries a generic `aa5g=0xf` field. `boardtype` is 0x0851 per radio and 0x0860 for the board; `boardnum=201222`.
- **6 GHz *(pass 2 correction)*:** the BCM43684 *does* have 6 GHz variants (`BCM43684_D11AX6G_ID 0x6006`, `…2G6G 0x6004`, `…5G6G 0x6005`, `…2G5G6G 0x6016` in `bcmdevs.h`), so the old claim "BCM43684 is a 2.4/5 GHz part" was wrong. The **HH4000** has no 6 GHz: its devids are 2G-only and 5G-only, and FCC VW3FAST5689 lists no 5925–7125 MHz operation (doc 10 #14) [V]. The **Giga Hub (FAST5689E)** is the same board with "6G Ant 0-3" added (doc 10 §2). Its 6 GHz chip is [U]; a fourth BCM43684 with a 6G devid would fit, but that is [I], unconfirmed.

### 1c. Interfaces [V] (kernel.log "Register interface")
Each radio exposes a primary interface plus up to 7 virtual APs (`wl0`, `wl0.1…wl0.7`, same for `wl1.x` and `wl2.x`). XMO defines 8 SSIDs: `WL_PRIV`, `WL_GUEST`, `WL_PLUME_2G4` on 2.4 GHz; `WL_DATA_5G`, `WL_GUEST_5G`, `WL_PLUME_5G` on RADIO5G; `WL_IPTV_5G`, `WL_DATA_5G2` on RADIO5G2. Base MACs are `<device-mac>/70/71`. Each VAP is also registered with WFD (`wfd_registerdevice Successfully registered dev wlN.M`).

### 1d. Dongle firmware & version [V]
- File: `etc/wlan/dhd/43684b0/rtecdc.bin` (2,382,510 bytes); an MFG variant is in `etc/wlan/dhd/mfg/43684b0/`.
- **Firmware version `17.10.121.41 (r796515)`**, build tag `43684b0-ram/config_pcie_fdap_release_airiq`, **Ucode Ver 1570.175**, **FWID 01-358cce7c**, CRC 5ce20161, dated `Fri 2022-03-25 14:50:12 CET` (strings in `rtecdc.bin`). `fdap` = FullMAC Dongle AP; `airiq` = AirIQ spectrum analysis. All three radios load the **same** image (`DHD concate_extfdimg_name: Final FW path is /etc/wlan/dhd/43684b0/rtecdc.bin`).
- *(Pass 2)* What the boot log says about the host/dongle protocol (kernel.log l.983–1030):
  - `PCIe IPC: … Rev host 0x82 dngl 0x82`, `FWID 0x358cce7c`, `rings h2d 530 d2h 6 flows 528 ifs 15`, `2 bytes sized indices`, `Host iDMA enable`, `DMAing indices: H2D 1 D2H 1`, `WI Formats TxP 1 RxP 1 TxC 1 RxC 1`.
  - HME (host memory extension) pages are allocated in host RAM (`HME: User #5 len 32, 131072`).
  - The firmware supports CSI Monitor (`CSIMON`).
  - The dongle RAM size is adjusted to 0x580000 (`dhdpcie_ramsize_adj`). `.bea` headers are absent.
- *(Pass 2 correction)* NVRAM **is downloaded to the dongle**: `dhdpcie_bus_write_vars: Download, Upload and compare of NVRAM succeeded`. Only the *file* path is empty (`nvram path=`). The variables come from the in-kernel `/proc/nvram` store, which dhd reads through `wlcsm_nvram_k_get`/`wlcsm_nvram_getall` (dhd.ko imports, §2) [V].
- MFG firmware is selected when `/proc/nvram/wl_nand_manufacturer` ∈ {2,3,6,7} (`bcm-base-drivers.sh`).

### 1e. NVRAM / calibration source [V]
- Per-radio calibration/SROM is in `ubi/calib/kernel_nvram.setting` (25,310 B) on the per-device `calib` partition. `/bin/load_kernel_nvram.setting` loads it into the Broadcom `/proc/nvram` store during S11. It contains:
  - pa/TSSI power tables (`nvram_params`: `pa0b0/b1/b2`, `pa0maxpwr`, per `pci/0/1/`, `pci/0/2/`…);
  - board params;
  - per-radio `devid`/`sromrev`/`aa*`/`rpcal*`/`pdgain5g`.
- `etc/wlan/kernel_nvram.setting` and `etc/wlan/default.kernel_nvram.setting_CA` are the ROM defaults. `etc/wlan/check_nvram/{version_nvram,md5_nvram}` integrity-check the stored copy.
- **Back up `calib` before any reflash.** It is per-device and cannot be regenerated.

### 1f. Country code & regulatory [V]
- Hard-coded **Canada**: `bcm-base-drivers.sh` runs `nvram set {1,2,3}:ccode=CA`, `{1,2,3}:regrev=0`, `regrev=0`, `nvram commit`, `nvram kcommit`. `S48wificountrycode` runs `wlctl -i wl{0,1,2} country CA/0`, then `/etc/checkwifi.sh &`.
- The calibration NVRAM ships `ccode=ALL` per radio, which is overridden at boot.
- Regulatory enforcement lives in the **dongle firmware** (FullMAC). The host cfg80211 only mirrors it (`cfg80211: Calling CRDA to update world regulatory domain` appears, but `CRDA` is not used to set power) [I].

---

## 2. Driver stack (stock) [V]

- **dhd.ko**: Broadcom **FullMAC Dongle Host Driver** (`Dongle Host Driver, version 17.10.121.41 (r796515)`, `Compiled in drivers/net/wireless/bcmdhd`). The 802.11 MAC/PHY runs as firmware on each dongle's ARM core; the host side is the PCIe MSGBUF ring protocol. License `GPL and additional rights`. Parameters `dhd_dpc_prio=5 iface_name=wl dhd_console_ms=0 firmware_path=/etc/wlan/dhd/`. One module drives all three dongles (`dhd:0:`).
- ***(Pass 2)* Runner DHD offload is enabled**: `dhd_runner_attach: Tx Offload - Enabled, Ring Size = 1024`, `Rx Offload - Enabled`, `Runner DHD PCIE: vendor<0x14e4> device<0xaaa4>`, `rdpa_dhd_helper_dhd_complete_ring_create`, `Runner DHD Offload initialization complete`. The BCM6858 Runner posts and completes the dongle's TX/RX flow rings directly. This is why dhd depends on `bdmf` and `rdpa_gpl`, and why the dhd0–2 bootmem pools exist.
- **dhd.ko imports (316 undefined symbols) grouped by provider [V]** (`nm -u dhd.ko` vs each module's defined symbols):

| Provider | # | Examples | Purpose | Needed by a port? |
|---|---|---|---|---|
| `hnd` | 11 | `osl_pktdup`, `linux_pktget`, `getvar`, `si_getdevpathvar` | OSL/HND utilities | No: source in impl87 `sys/src/shared/` is compiled into DHD |
| `wlcsm` (Prop) | 3 | `wlcsm_nvram_k_get/k_set/getall` | kernel NVRAM | No: only via `nvram_ext.c` (BCA_HNDROUTER); use an NVRAM file |
| `emf` / `igs` (Prop) | 5 / 4 | `emfc_input`, `igsc_init` | WMF multicast-to-unicast, IGMP snooping | No: only `dhd_wmf_linux.c` (`-DDHD_WMF`) |
| `wfd` | 7 | `wfd_bind`, `wfd_registerdevice` | Runner WiFi forwarding | No: `-DBCM_WFD` only if `CONFIG_BCM_WIFI_FORWARDING_DRV` |
| `bdmf`, `rdpa_gpl` | 11 / 17 | `rdpa_dhd_helper_*`, `rdpa_cpu_*` | Runner DHD offload | No: `-DBCM_DHD_RUNNER` only if `CONFIG_BCM_DHD_RUNNER` |
| `bcmlibs` (Prop) | 15 | `d3lut_*`, `pktlist_context_*` | PKTFWD lookup/packet lists | No: `CONFIG_BCM_PKTFWD` |
| `bcmmcast` | 1 | `bcm_mcast_wlan_client_disconnect_notifier` | multicast accel | No |
| `cfg80211` | 33 | `cfg80211_external_auth_request`, `cfg80211_ch_switch_notify`, `cfg80211_inform_bss_width_frame`, `__cfg80211_alloc_reply_skb` | wiphy/nl80211 | **Yes** (kernel's own cfg80211) |
| vmlinux | rest | | | |

- **wfd.ko**: "WLAN Forwarding Driver". It binds each `wlN` to a Runner queue pair and a `wfdN-thrd` (`wfd_bind: Dev wl0 wfd_idx 0 … Type fkb:sll …`), which gives the accelerated LAN↔WLAN path (02/11). GPL.
- **hnd.ko / wlcsm.ko / emf.ko / igs.ko**: HND utilities (GPL+additional rights); config/NVRAM manager (**Proprietary**, no source anywhere in the Merlin repo [V]); EMF/IGS multicast (**Proprietary**).
- **cfg80211.ko**: Broadcom's build of the 4.1 cfg80211 **with backports**. It exports `cfg80211_external_auth_request` (a mainline 4.17 API), `cfg80211_ch_switch_started_notify`, `cfg80211_ft_event`, `cfg80211_crit_proto_stopped`, `cfg80211_cac_event`, and 225 functions in total. The Merlin 4.1 `net/wireless/nl80211.c` carries the same external-auth backport [V]. dhd registers wiphys through it, plus a vendor80211 interface (`wl_setup_wiphy : Registering Vendor80211`).
- **wl.ko** (SoftMAC NIC driver) is referenced by the SDK script but **absent** [V].
- **hostapd / wpa_supplicant**: `hostapd v2.9` and `wpa_supplicant v2.9`, 32-bit ARM glibc builds (05 §5) with Broadcom patches ("Unknown Broadcom information element ignored"), nl80211 driver (517 `nl80211` strings). `S90hostap` starts both in global-control-socket mode (`hostapd -g /var/run/hostapd/global -B -P …`, `wpa_supplicant -g /var/run/wpa_supplicantglobal -B -s`). hg6d and OpenSync `wm` push per-BSS config over the socket. So Sagemcom **runs hostapd on top of dhd's cfg80211 FullMAC AP path**, not mac80211.
- **Userspace tools**: `wl`/`wlctl` (symlink), `wlconf`, `halwifi` (contains `insmod dhd dhd_dpc_prio=5 iface_name=wl dhd_console_ms=0`), `acsd2`, `eapd`, `wps_monitor`, `bsd` (band steering), `wlaffinity`, `dhd`/`dhdctl`. All are 32-bit.
- **Mesh / EasyMesh / Plume**: no 802.11s and no wpa_supplicant mesh. Multi-AP ("Wi-Fi pods") is **Plume OpenSync** (`wm`/`bm`/`sm` managers + cloud, `bsd` steering, `WL_PLUME_*` backhaul VAPs). See 05 §3.

---

## 3. OpenWrt feasibility for WiFi

**Upstream status [V]:**
- **brcmfmac:** no BCM43684. `brcm_hw_ids.h`, `brcmfmac/pcie.c` and `brcmfmac/chip.c` on torvalds/linux master have **0** matches for `43684|aaa4|0x4429|0x442a|0x442b` (fetched 2026-11-04; last `brcm_hw_ids.h` commit `74e2ef72`, 2025-07-24; local copies in `$P/work/wifi/upstream/`). The nearest supported AP-class chips are 4366B/C (`brcmfmac4366b/c-pcie`).
- **linux-firmware:** `WHENCE` on gitlab `kernel-firmware/linux-firmware` main has **0** matches for `43684|4368|aaa4`. The only brcm AP-class entries are `brcm/brcmfmac4366b-pcie.bin` and `brcm/brcmfmac4366c-pcie.bin`.
- **Protocol mismatch, even if you added the ID:** brcmfmac accepts PCIe shared-memory versions `BRCMF_PCIE_MIN_SHARED_VERSION 5` … `MAX 7` (`pcie.c:216-218`). This firmware advertises BCA IPC revision **0x82** (`PCIE_IPC_BCA_REV 0x80 | 2`; `bcmpcie.h:160-167`; log `Rev host 0x82 dngl 0x82`). It also expects BCA features brcmfmac lacks: HME host memory, iDMA indices, the ACWI work-item formats, and 530 H2D rings. Expect brcmfmac to reject it, and wrapping it would amount to rewriting brcmfmac's PCIe layer [I, strong].
- **OpenWrt:** no bcmbca/6858 target. `bcm4908` (kernel 6.18) has no 43684 WiFi either (doc 10 #18).

**Options, in order of effort:**

| Path | Kernel | WiFi | Effort | Notes |
|---|---|---|---|---|
| **A. Stock kernel + stock WiFi modules** | stock 4.1.52 | all 3 radios, accelerated | low | Replay 05 §2c phase 1 (`cfg80211`, `bcmlibs`) and the 05 §2d W-steps: `/proc/nvram` calibration, `ccode`, `wfd hnd emf igs dhd`, `wlctl country CA/0`. Works with an OpenWrt userspace (05 §8). OpenWrt's `wifi`/netifd scripts only handle mac80211 and broadcom-wl, so either use the stock 32-bit `hostapd`/`wl`/`acsd2` in a compat directory with your own init script, or try OpenWrt's hostapd 2.11 (nl80211) against dhd's cfg80211 [U] |
| **B. Modern kernel + ported DHD** | 6.6/6.12 | all 3 radios, not accelerated | high (§4) | Needs a 6858 PCIe RC driver first (mainline `pcie-brcmstb` has only `brcm,bcm4908-pcie`; doc 10 §3) |
| **C. Modern kernel, internal radios dark** | any | none, or a USB mt76/ath dongle via mac80211 | lowest for WiFi | Stock kernel has `MAC80211` unset, so a USB dongle also needs a modern kernel or a mac80211 build for 4.1 [I] |

- **Calibration is per-device and essential** on every path. The pa/TSSI tables in `calib` must be re-injected: through `/proc/nvram` on path A, or as per-radio NVRAM text files on path B. Without them TX power and regulatory behaviour are wrong.
- **Reusing the stock `dhd.ko` on a modern kernel is not possible** (vermagic `4.1.52-5.02L.07`, 4.1 struct layouts, Broadcom-only imports).

---

## 4. Porting DHD to a modern kernel with the stock `rtecdc.bin` (pass 2)

### 4a. Matching source exists [V]
| Tree (ASUS-Merlin) | DHD source? | Version |
|---|---|---|
| `src-rt-5.02L.07p2axhnd` (our SDK, kernel 4.1) | **No**: `impl69/sys/src/dhd/` contains only `exe/prebuilt/dhd`; the driver ships as prebuilt `router-sysdep.{rt-ax86u,gt-axe11000}/hnd_extra/prebuilt/dhd.o` (BCM4908 boards) | – |
| `src-rt-5.04axhnd.675x` (kernel 4.19) | **Yes**: `bcmdrivers/broadcom/net/wl/impl87/sys/src/dhd/sys/` (25 `.c`, 58,140 lines), cfg80211 glue `impl87/sys/src/wl/sys/wl_cfg80211.c` (26,160 lines), `wl_cfgp2p.c`, `wl_cfgvendor*.c`, shared HND/OSL in `impl87/sys/src/shared/`, BCA glue in `wl/shared/impl1/` (`dhd_runner.c`, `dhd_wfd.c`, `dhd_nbuff.c`, `dhd_blog.c`, `dhd_pktfwd.c`), build rules `impl87/sys/src/dhd/config/dhd_router.mk` | `EPI_VERSION_STR "17.10.188.6401 (r808804)"` (`impl87/sys/src/include/epivers.h`) |

- **Same branch as the stock firmware** (17.10.x).
- **IPC compatibility:** the host revision is `PCIE_IPC_REVISION = 0x80 | PCIE_IPC_VERSION (5)` = **0x85**. `dhdpcie_ipc_rev_compatible()` accepts any BCA dongle revision with `host_rev >= dngl_rev` (`dhd_pcie.c:7335-7365`), and the stock firmware is **0x82**, so it is accepted by design [V code]. Rev 0x83–0x85 features (variable HME users, 2560-byte ioctl buffers) are negotiated [I].
- Kernel coverage: `LINUX_VERSION_CODE` checks reach `KERNEL_VERSION(5,10,0)` in the DHD files and `(5,8,0)` in `wl_cfg80211.c`, so the code was maintained up to about 5.10 [V].

### 4b. Which dependencies are optional [V `dhd_router.mk`]
| Dependency | How it enters the build | Turn off by | Replacement |
|---|---|---|---|
| **wlcsm** (proprietary, no source) | `shared/nvram_ext.c` → `wlcsm_nvram_k_get/set/getall`, compiled only under `BCA_HNDROUTER`; also used by `dhd_nbuff.c`/`dhd_wfd.c` | build without `BCA_HNDROUTER` (Broadcom's own `CONFIG_STBAP` branch, which adds `-DBCM_REQUEST_FW` and `linux_pkt.c`) | per-radio NVRAM text via `nvram_path`/`request_firmware` (`dhdpcie_download_nvram()`, `dhd_pcie.c:2000`), generated from `kernel_nvram.setting` |
| **hnd** | OSL/siutils symbols | always: DHD compiles `src/shared/{linux_osl,aiutils,siutils,bcmutils,hndpmu,bcmevent,…}.c` itself | – |
| **emf / igs** (proprietary) | `-DDHD_WMF` (unconditional in `dhd_router.mk`), `dhd_wmf_linux.c` | drop `-DDHD_WMF -DDHD_IGMP_UCQUERY -DDHD_UCAST_UPNP` | none needed (bridge IGMP snooping) |
| **wfd** | `-DBCM_WFD` iff `CONFIG_BCM_WIFI_FORWARDING_DRV` or Archer | leave unset | normal netdev RX |
| **Runner offload** (`bdmf`/`rdpa_gpl`) | `-DBCM_DHD_RUNNER` iff `CONFIG_BCM_DHD_RUNNER` | leave unset | host-managed flow rings; no dhd0–2 bootmem needed, normal coherent DMA (cmdline already has `coherent_pool=4M`) [I] |
| **NBUFF/FKB, BLOG/flow cache** | iff `CONFIG_BCM_KF_NBUFF`, `CONFIG_BLOG` (BCM kernel patches) | absent on a mainline kernel | plain `sk_buff` |
| **PKTFWD / CPEPKTC (bcmlibs d3lut)** | iff `CONFIG_BCM_PKTFWD` / `CONFIG_BCM_CPEPKTC` | leave unset | – |
| **bcmmcast** | via WMF/BLOG | as above | – |
| **cfg80211** | `CONFIG_BCM_HOSTAPD=y` → `-DWL_CFG80211 -DWL_SAE -DWL_HAPD_WDS …` + `wl_cfg80211.c` | **required** for hostapd | mainline cfg80211 |
| embedded firmware | `-DBCMEMBEDIMAGE="rtecdc_router.h"` unless `CONFIG_STBAP` or `BCM_EXTFDIMAGE_PATH` | `BCM_EXTFDIMAGE_PATH` / `BCM_REQUEST_FW` | load the stock `rtecdc.bin` (the stock driver already uses `firmware_path`) |

### 4c. Remaining work and risks [I]
1. **PCIe RC on the new kernel.** No 6858 host driver exists in mainline, and Merlin has no source (05 §7a). Extend `pcie-brcmstb` (it has `brcm,bcm4908-pcie`) with a 6858 config, PMB power (`PCIE0 bus1/dev18`, `PCIE1 bus1/dev19`; the PCIE2 domain is [U]), and PERST. See doc 10 §3. This is a **prerequisite**.
2. **cfg80211 API forward-port from ~5.10 to 6.6/6.12.** The known churn is MLO `link_id` arguments (`start_ap`/`stop_ap`, `change_beacon` → `cfg80211_ap_update` in 6.7, `ch_switch_notify`, `get_channel`, `add_key`/`del_key`), `del_station` params, `cfg80211_rx_mgmt` and `cfg80211_ch_switch_notify` signatures, and `wireless_dev` link structures. The surface is `wl_cfg80211.c` (26k lines), most of which is P2P/STA code you can `#ifdef` out for an AP-only build.
3. **Core-kernel API churn in DHD (58k lines):** timers (`timer_setup`, done by 4.15), `netif_rx_ni` removal (5.18), `dev_addr` const (5.17), `in_interrupt` usage, `pci_*` DMA API removal (5.18), `proc_ops` (5.6), kthread and workqueue changes.
4. **Calibration NVRAM:** convert `kernel_nvram.setting` (`1:`/`2:`/`3:` prefixes and `pci/0/N/` devpaths) into the per-radio key=value files the dongle receives today. Byte-compare against `dhdpcie_bus_write_vars`' upload-and-compare path [I].
5. **Firmware/host feature negotiation:** the stock build ran with `BCMHWA`, HME, iDMA and CSIMON. Without `BCMHWA`/HME support compiled in, the dongle may refuse to start or run degraded [U]. Keep those flags; they are host-side only and independent of BCA.
6. **Throughput:** without Runner offload all WLAN traffic goes through the B53 CPUs. Expect well below the stock multi-gigabit aggregate [I].
7. **Licensing:** DHD is "GPL and additional rights" (dual-licensed Broadcom source), so a port is redistributable. The firmware blob `rtecdc.bin` is Broadcom-proprietary; keep it on-device.

**Feasibility verdict [I]:** porting DHD itself is **realistic for an experienced kernel developer**. Matching-branch source exists, the IPC check accepts the stock firmware, and every proprietary dependency can be turned off at build time. It is still a large job: weeks for a 5.10/5.15 kernel, more for 6.6/6.12, and it **cannot start until a modern-kernel 6858 PCIe RC driver exists**. Path A (stock kernel) is the only route that gives working WiFi now.

---

## Evidence index
- vermagic/deps/params: `modinfo lib/modules/4.1.52-5.02L.07/dhd.ko`; imports: `aarch64-linux-gnu-nm -u dhd.ko`.
- PCIe/chip/IPC: `ubi/fs_filesystem1/data/kernel.log` l.572–654 (bridges/dongles), l.955–1310 (dhd attach, IPC, Runner offload, revinfo).
- Device IDs: `$P/work/os/merlin/release/src-rt-5.0{2L.07p2axhnd/…/impl69,4axhnd.675x/…/impl87}/main/src/include/bcmdevs.h:308-314,570`.
- Band plan: `ubi/calib/kernel_nvram.setting`; `ubi/fs_filesystem1/conf/backup-cfg-WiFi-31.xml` (`Radio`/`SSID`/`AccessPoint` uids); `rootfs/etc/checkwifi.sh` l.55–62.
- Firmware version: `strings etc/wlan/dhd/43684b0/rtecdc.bin`.
- Load order / country: `etc/bcm-base-drivers.sh`, `etc/init.d/wificountrycode`, `etc/init.d/hostap`.
- DHD source: `$P/work/os/merlin/release/src-rt-5.04axhnd.675x/bcmdrivers/broadcom/net/wl/{impl87/sys/src/{dhd,wl,shared,include},impl87/sys/components/proto/include/bcmpcie.h,shared/impl1}`.
- Upstream: [torvalds/linux brcm80211](https://github.com/torvalds/linux/tree/master/drivers/net/wireless/broadcom/brcm80211) (`include/brcm_hw_ids.h`, `brcmfmac/pcie.c`, `brcmfmac/chip.c`), [linux-firmware WHENCE](https://gitlab.com/kernel-firmware/linux-firmware/-/blob/main/WHENCE), [kernel.org brcm80211 driver page](https://wireless.docs.kernel.org/en/latest/en/users/drivers/brcm80211.html).

---

## Revision log (pass 2, 2026-11-04)

| # | Section | Previous claim | Pass-2 result | Evidence |
|---|---|---|---|---|
| 1 | §1a | dongles at `000N:00:00.0`; "`aaa4` = Broadcom PCIe dongle class ID (BCM4368x family)" | **Corrected [V]:** dongles at `000N:01:00.0` behind root ports `[14e4:6858]`; `0xaaa4` = 43684 decimal = `BCM43684_CHIP_ID` | kernel.log, `bcmdevs.h` |
| 2 | §1b band | "2.4 / 5 / 5 inferred from antenna masks [I]" | **[V]:** devid `0x442a` = `D11AX2G`, `0x442b` = `D11AX5G`; XMO `SplittedOperatingFrequencyBand` LOWER/UPPER. wl1 = upper, wl2 = lower is **[I]** (checkwifi.sh AP mapping) | §1b |
| 3 | §1b 6 GHz | "BCM43684 is a 2.4/5 GHz 11ax part" | **Corrected [V]:** 43684 has 6G device IDs; HH4000 has none (devids, FCC); Giga Hub 6 GHz chip [U] | `bcmdevs.h`, doc 10 |
| 4 | §1b | "[I for the exact 5L/5H channel split]" | **Resolved [V config]:** LOWER default ch 44/80 MHz, UPPER ch 149; DFS disabled in config | WiFi-31.xml |
| 5 | §1d/§1e | "NVRAM supplied in-kernel, not via file" | **Refined [V]:** NVRAM is written to the dongle (`dhdpcie_bus_write_vars … succeeded`), sourced from `/proc/nvram` via wlcsm imports | kernel.log, `nm -u` |
| 6 | §2 | dhd deps listed | **Expanded [V]:** Runner DHD offload enabled; 316 imports grouped by provider | §2 table |
| 7 | §2 cfg80211 | "Broadcom's build of the 4.1-era cfg80211" | **Refined [V]:** 4.1 cfg80211 with backports (exports the 4.17 `cfg80211_external_auth_request`) | `nm cfg80211.ko`, Merlin `nl80211.c` |
| 8 | §2 hostapd | v2.9 | **Confirmed [V]**, plus wpa_supplicant v2.9; both 32-bit | `strings`, `file` |
| 9 | §3 brcmfmac | "[I, high-confidence … unverified by citation]" | **[V]:** no 43684/aaa4 in brcmfmac (master, 2026-11-04) or linux-firmware WHENCE; plus the IPC rev 0x82 vs shared rev 5–7 mismatch [V code, I outcome] | §3 |
| 10 | §3 | "Reusing stock dhd.ko on mainline: not feasible" | **Confirmed [V]**; replaced the binary verdict with paths A/B/C | §3 |
| 11 | new §4 | – | DHD source 17.10.188.6401 in Merlin 5.04 impl87; IPC-compatible with fw 0x82; optional-dependency matrix; port effort | §4 |
| 12 | §1c | "Bell main SSID, guest, IoT, Plume backhaul, etc." | **Replaced [V]** with the actual 8 XMO SSIDs | WiFi-31.xml |

Open items: wl1/wl2 sub-band assignment confirmed only via checkwifi.sh [I]; OpenWrt hostapd 2.11 against dhd's cfg80211 [U]; dongle behaviour without HWA/HME host support [U]; Giga Hub 6 GHz chip [U]; PCIE2 PMB domain [U].
