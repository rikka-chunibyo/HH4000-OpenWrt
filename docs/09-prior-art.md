# 09 - Prior art: open-source PON work and what carries over to the HH4000

Research date: 2026-11-03. Follow-up to 08-pon-reimplementation.md.
Question: has anyone built an open PON stack, and can parts of it be reused here?

Legend: **[V]** verified (source linked or checked on our files), **[I]** inferred, **[U]** unverified.

---

## 0. Summary

- **Nobody has published open PON (or open Runner/XRDP) work for Broadcom BCA chips** that I could
  find. Searches for Broadcom GPON stack replacements, BCM6858/68580 OpenWrt ports and mainline
  Runner drivers turned up nothing beyond device-tree/SPI/NAND support. The HH4000 would be first.
- **Two independent open GPON ONU stacks do exist and work against real OLTs in 2026:**
  1. **odi-oss** (Realtek RTL9602C SFP stick): own GPL kernel GPON MAC driver + own OMCI daemon,
     reaches O5 and carries PPPoE on a live ISP line.
  2. **OpenWrt EcoNet EN7528** (draft PR #24577): kernel xPON MAC + optics driver + new userspace
     OMCI daemon, O5 + OMCI + PPPoE on a live Nokia/DZS line.
- **What carries over:** the OMCI daemon (the largest software piece), PLOAM state-machine logic,
  OLT-interop lessons, test methodology, and possibly a common OpenWrt kernel↔userspace interface.
  **What doesn't:** the MAC drivers (chip-specific; our extracted register DB covers that, doc 08)
  and the datapath (Runner), which nobody has opened.
- **Useful local find [V]:** the stock Broadcom OMCI library implements **106 managed entities (path strings; 83 OMCI-addressable classes, see doc 12)
  including four Huawei-specific MEs (65408, 65414, 65427, 65440)** and has `isHuaweiOLT`
  detection. Your logs show a Huawei OLT, so that inventory is the target MIB (§4).

---

## 1. Projects found

| Project | Chip | Licence | What it is | Status | Reuse for HH4000 |
|---|---|---|---|---|---|
| [odi-oss](https://github.com/AndreZiviani/odi-oss) | Realtek RTL9602C (Lexra MIPS) | GPL-2.0-or-later (own code) | Linux 6.18 firmware: built-in GPL switch + GPON MAC driver, `/proc/odi_gpon/{status,counters,ploam,flows}`, own `omcid` + `omcicli`/`omciprobe`/`omcicap`, qemu replay of OMCI captures from 2 ISPs | O5 + MIB upload compared vs stock on 2 OLTs; 3 h soak; PPPoE in daily use; many items host-tested only. README says the code is AI-assisted ("vibecoded") | **High**: OMCI daemon structure, MIB modelling, PLOAM logic, test harnesses. MAC/switch layer not reusable |
| [open-dfp-34x-2c](https://github.com/saquili/open-dfp-34x-2c) | Realtek RTL9601D/9602C | GPL-2.0-only | Separate open firmware with own `omcid/` (G.988: GEM, T-CONT, VLAN translation, VEIP, alarms, MIB upload) | Test builds; lab OLT; first release Oct 2026 | Medium: second OMCI reference |
| [OpenWrt PR #24577](https://forum.openwrt.org/t/openwrt-support-for-zyxel-pmg5617ga-and-econet-socs-first-gpon-support/144231?page=21) (AKoo7) + [airoha_kernel](https://github.com/Sirherobrine23/airoha_kernel) (Sirherobrine23) | EcoNet/Airoha EN7528/EN7523 | MAC ported from EcoNet's GPL `tclinux_phoenix` source | `kmod-econet-xpon` (GPON MAC + EN7571 optics, `ponwan0` netdev, GEM/T-CONT, activation SM), `econet-omcid` (programs datapath via `/proc/econet_xpon_*`), UCI config, LuCI app | Draft. O5 + OMCI + PPPoE on live Nokia/DZS; models one baseline MIB, "not yet a complete generic OMCI stack" | **High (strategic)**: the first PON support likely to land in OpenWrt. Aligning with its userspace interface (§3) could let the HH4000 share an OMCI daemon and LuCI app |
| [omci-lib-go](https://github.com/opencord/omci-lib-go) (ONF/opencord) | – | Apache-2.0 [I] | G.988 message codec (baseline + extended message set, 2020 amendments), relaxed decoding, unknown/vendor ME passthrough | Mature, used by VOLTHA | Medium: decode stock-firmware OMCI captures, build replay tests; basis for a Go daemon if desired |
| [onumock](https://pkg.go.dev/github.com/cboling/onumock) / BBSim [omci-sim](https://gerrit.lfbroadband.org/plugins/gitiles/omci-sim/) | – | Apache-2.0 [I] | ONU-side simulators with canned MIBs for OLT scale tests | Test tools | Low–medium: ONU-side MIB/response structure |
| VOLTHA openonu adapter / pyvoltha [OpenOMCI](https://gerrit.lfbroadband.org/plugins/gitiles/voltha/+/4069a510492cd707a32cb1078df1de861ddbb66d/voltha/extensions/omci/openomci_agent.py) | – | Apache-2.0 | **OLT-side** OMCI management (MIB sync/upload state machines from the OLT's point of view, ME class models) | Mature | Medium: shows what an OLT expects; ME definitions. Not an ONU stack |
| [realtek-libohwtc](https://github.com/tripleoxygen/realtek-libohwtc) | Realtek Luna SDK | GPL-2.0 | Registers Realtek SDK "feature 0x14" so Realtek's closed OMCI loads its Huawei ME libraries (`mib_Me350/370/373.so`); the handler is a stub | Small | Low: confirms Huawei OLTs want vendor MEs; contains no ME payloads |
| [8311 WAS-110 firmware](https://github.com/djGrrr/8311-was-110-firmware-builder) | MaxLinear PRX126/PRX300 | GPL build system; MaxLinear `omcid` is closed (binary-patched) | Community firmware for the WAS-110 XGS-PON stick; exposes equipment ID, ONU2-G, MIB `.ini` selection | Mature, widely used | Low for code; useful as an **external ONT** (doc 03 §6) and for MIB-file ideas |
| [Anime4000/RTL960x](https://github.com/Anime4000/RTL960x), [hack-gpon.org](https://hack-gpon.github.io/ont) | Mostly Realtek/Lantiq/MaxLinear | – | Community knowledge on OLT-vendor quirks, ONT sticks | – | Reference |
| [FPGA-ONU-1GPON-1GE](https://github.com/alphaonex86/FPGA-ONU-1GPON-1GE) | FPGA | CERN-OHL-S-2.0 | Open GPON/EPON MAC in SpinalHDL (GTC framing, FEC, AES, PLOAM, DBA) | Hardware project | Low: TC-layer reference only |

---

## 2. Assessment of the suggestions you pasted

| Suggestion | Verdict | Notes |
|---|---|---|
| ASUSWRT-Merlin GPL code as the base | **Partly right; already in use** | Doc 02 §8–9 uses the Merlin `src-rt-5.02L.07p2axhnd` tree. It has GPL bcm_enet, LPORT/PHY/SerDes, boardparms and the **XRDP_CFE minimal 6858 Runner datapath**. Correction: the full Runner (`rdpa`) and PON stacks are **binary** in those trees too, and the 6858 WAN driver there (`wan_drv_impl1/2.c`) targets older chips (doc 08 §2.4) |
| "ap630-debian" project with an rdp-init approach (BCM4906/4908) | **Couldn't verify, disregard for now** | No repository by that name or with "rdp-init" turned up. The premise also doesn't transfer: BCM4908 Ethernet works in mainline (`bcm4908_enet`) through a plain DMA engine. On the BCM6858, `bcm_enet` has **no Runner-free DMA path** (doc 02 [V]), so there is no "slow path" to fall back to. If you have a link, I'll look again |
| VOLTHA OpenOMCI as a ready OMCI stack | **Exists, but it's OLT-side** | OpenOMCI manages ONUs from the network side. Its "Broadcom" handling is an adapter for *managing* Broadcom-based ONUs (e.g. "brcm onu won't upload once provisioned"). Useful for ME models and to understand OLT behaviour; it does not replace an ONU stack. The ONU-side starting points are odi-oss / econet-omcid |
| realtek-libohwtc and "0x14" for Huawei MEs | **Exists, overstated** | 0x14 is a Realtek SDK feature-registration code, not an OMCI value. The library only flips that feature on; the ME implementations live in Realtek's closed `mib_Me3xx.so`. The useful takeaway (Huawei OLTs may need vendor MEs) is confirmed better by our own stock library (§4) |
| Broadcom OpenNSA gpl-module for SerDes/DMA | **Not applicable** | OpenNSA is for StrataXGS switch ASICs (Trident2/Tomahawk), and the repo was archived Jan 2026. Its `gpl-module` is the kernel BDE/KNET shim (PCI access, DMA buffers, packet I/O); the SerDes code is in the closed SDK. The BCA chips use different SerDes code (Merlin16/Eagle), whose GPL and RU sources we already have |

---

## 3. What to reuse, concretely

### 3.1 OMCI daemon (biggest win)
- Start from **odi-oss `omcid`** (GPL-2.0-or-later, so compatible with a GPL HH4000 port) or
  **econet-omcid**, and give it an HH4000 hardware backend. The daemon logic (MIB database, MIB
  upload, create/set/get handling, alarms, software image, VLAN/bridge model) is chip-independent.
  The backend maps OLT-created GEM ports, T-CONTs, priority queues and VLAN rules onto our hardware:
  the Runner (L3 in doc 08) plus the NG/GPON MAC tables (Alloc-ID, Port-ID).
- Target MIB = the **stock inventory in §4**, so the OLT sees the same ONU it saw before.
- odi-oss already supports private MEs 350/370/373 (`OMCI_PRIVATE_ME`), VEIP or PPTP-only modes and
  a fixed-reply fallback for unmodelled classes (`OMCI_FIXED_ME`). Those are exactly the knobs a
  Huawei-OLT deployment needs.

### 3.2 PLOAM / ONU state machine
- odi-oss's kernel driver implements G.984.3 states O1–O7 with fixes found on live OLTs. The
  register access is Realtek-specific, but the state logic transfers. With our extracted
  `GPON_RX_*`/`GPON_TX_*` register DB (doc 08 §2), the BCM6858 side is a "swap the register layer"
  exercise for GPON. XGS-PON (G.9807.1) has no open ONU reference yet.

### 3.3 Interop lessons from these projects
- **PLOAM addressing:** act on directed messages only when they carry our ONU-ID (or broadcast),
  and only in the right states (odi-oss PR #87, #161).
- **Repeated messages:** OLTs repeat some PLOAMs (e.g. Extended_Burst_Length every ~5.7 s in O5);
  apply such fields only in the state the standard says (O3).
- **Key switching:** overwrite stale Key_Switching_Time in hardware on every new key request.
- **Data GEM never created:** Nokia/DZS OLTs read ME 277 (Priority Queue) and 278 (Traffic
  Scheduler) in MIB upload and reject partial attribute masks. Return full masks (`0xfff0`/`0xf000`)
  with pointers matching the real queue/T-CONT topology (OpenWrt EcoNet thread).
- **The ISP gate is off-device:** the OLT only provisions service for an ONU serial the ISP has
  registered. That's fine here, since it's your own HH4000's identity on your own line.

### 3.4 Test methodology
- **OMCI trace corpus:** record the full OLT↔ONU exchange from the stock firmware (doc 08 §4 step 3),
  decode it with omci-lib-go, and replay it against the new daemon offline. odi-oss does this
  under qemu, and the OpenWrt maintainers have asked for the same thing as a regression corpus.

### 3.5 Kernel↔userspace interface
- The OpenWrt EcoNet effort is still deciding between `/proc` files and Generic Netlink + sysfs,
  and how much OMCI belongs in the kernel. If the HH4000 driver exposes **the same interface and
  UCI config** that OpenWrt settles on, one OMCI daemon and one LuCI app could serve both platforms.
  Worth following PR #24577 before designing the HH4000 interface.

---

## 4. Stock OMCI managed-entity inventory [V]

Extracted from `work/rootfs/usr/lib/libomci_core.so` (Broadcom OMCI v8.42 MDM object paths,
`InternetGatewayDevice.X_ITU_T_ORG.G_988.<group>.<ME>.{i}.`). Saved to
`work/pon/omci/stock_me_inventory.txt`. **106 MEs** (path strings; doc 12 found 83 OMCI-addressable classes, 22 voice MEs exist only as names, 13 objects have no class ID):

| Group | Count | Notable MEs |
|---|---|---|
| AniManagement | 16 | AniG, TCont, GemPortNetworkCtp, GemInterworkingTp, MulticastGemInterworkingTp, GalEthernetProfile, FEC/TC/XG-PON PM history data |
| EquipmentManagement | 16 | OnuG, Onu2G, OntData, SoftwareImage, CardHolder, CircuitPack, PortMappingPackageG, OnuDynamicPowerManagementControl, OnuRemoteDebug, **HuaweiSpecificMe65408 / 65414 / 65427 / 65440** |
| Layer2DataServices | 22 | MacBridgeServiceProfile, MacBridgePortConfigData, MapperServiceProfile (802.1p), VlanTaggingFilterData, ExtendedVlanTaggingOperationConfigurationData, MulticastOperationsProfile, Dot1RateLimiter, Ethernet frame PMs |
| EthernetServices | 5 | PptpEthernetUni, **VirtualEthernetInterfacePoint** (VEIP, the mode this unit ran) |
| TrafficManagement | 3 | PriorityQueueG, TrafficSchedulerG, GemTrafficDescriptor |
| VoiceServices | 20 | PptpPotsUni and SIP/VoIP config MEs |
| General / Layer3 / Misc / MoCA | 13 / 5 / 3 / 3 | TR069ManagementServer, IP host config, video ANI/UNI, MoCA |

Vendor handling found in the OMCI libraries [V]: `isHuaweiOLT`, `OltVendorId`, "Huawei OLT detected"
(`libomci_swan.so`), Nokia 7342 OLT detection, and `huaweiSpecificMe65408ParamArray`-style
attribute tables. The **Huawei ME attribute layouts are in those param arrays**: decoding them
(names, types, sizes, access) is the next concrete step for a compatible MIB.

---

## 5. Effect on the plan (doc 08 §6)

1. Runner/XRDP is still the critical path, and **nobody else has opened it**. That remains our own work.
2. The PON software layers shrink substantially: adapt an existing GPL OMCI daemon instead of
   writing one, and port proven GPON state-machine logic onto our register DB.
3. Revised GPON-only order: (a) Runner minimal datapath (shared with Ethernet), (b) WAN SerDes +
   GPON MAC bring-up by register-snapshot replay, (c) port odi-oss-style PLOAM SM, (d) adapt
   odi-oss/econet `omcid` with an HH4000 backend and the §4 MIB, (e) validate against the recorded
   stock OMCI trace before touching the live line.
4. Next research steps: decode the Huawei ME param arrays in `libomci_core.so`; check odi-oss's
   `omcid` source language and structure before choosing a base; follow OpenWrt PR #24577 for the
   interface decision.
