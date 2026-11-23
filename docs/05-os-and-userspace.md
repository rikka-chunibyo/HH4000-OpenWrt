# 05 - Operating System & Userspace (HH4000 / FAST5689 / BCM6858)

Scope: kernel, boot/init, Sagemcom middleware, services, users/credentials/remote
access, userspace ABI, persistence options, kernel rebuild feasibility and
OpenWrt userspace compatibility. Firmware analyzed:
`4KOS_20.2.152_prod` ("scOS SGC821012E"), built 2022-04-15.
Model strings `FAST5689` / `Home Hub` / `SGC821012E` (Verified: strings in
`usr/bin/hg6d`, rootfs `/etc`).

Legend: **[V]** Verified (path/string/log/source cited) · **[I]** Inferred · **[U]** Unverified / unknown.
`$P` = `/path/to/HH4000-OpenWRT`. Pass-2 scratch: `$P/work/os/pass2/`, scripts `$P/work/os/scripts/OS_*`.

**Pass-2 summary (the three questions that matter for a homelab OpenWrt build):**
1. **Kernel rebuild from the ASUS-Merlin GPL tree: not possible as a bootable 6858 kernel, but works for out-of-tree modules.** The Merlin 4.1.52 tree has the core kernel and `CONFIG_BCM96858`. It ships the board, PMC, PCIe host, arm64 setup and other BSP drivers only as **BCM4908-built prebuilt `.o` files** (RT-AX86U/GT-AXE11000 `hnd_extra/prebuilt/`), with no source. Modules built from it against the stock config are **ABI-compatible** for every structure checked once one ASUS-only `nf_conn` field is removed, and the vermagic matches exactly (§7) [V].
2. **Newest usable OpenWrt userspace: 24.10, and very likely 25.12, on the stock 4.1 kernel, provided you use fw3 (iptables) instead of fw4.** fw4 needs nf_tables with inet-family NAT, i.e. kernel ≥ 5.2. procd/netifd/ubus/fstools run without namespaces, cgroups or seccomp (those features are skipped silently). fw3 needs one rebuilt module, `xt_comment.ko`, which pass 2 built (§8) [V source, I runtime].
3. **WiFi on a modern kernel** depends on porting Broadcom's DHD. Source exists (impl87, 17.10.188, same branch as the stock firmware). See 06 §4.

---

## 1. Kernel

- **Version [V]:** `Linux version 4.1.52-5.02L.07 (rmm-svce-urd2int05@compil-atr-2) (gcc version 5.5.0 (crosstool-NG crosstool-ng-1.23.0-317-g85b86d8)) #1 SMP PREEMPT Fri Apr 15 16:48:43 CEST 2022` (`strings work/vmlinux.bin`). Broadcom BCA SDK **5.02L.07**, ARM64 kernel.
- **vermagic (module ABI) [V]:** `4.1.52-5.02L.07 SMP preempt mod_unload aarch64`. All **143/143** `.ko` carry exactly this string (`modinfo -F vermagic`, `work/os/pass2/modinv.tsv`). `CONFIG_MODVERSIONS` and `CONFIG_MODULE_SIG` are off, so only the vermagic string gates `insmod`, along with real struct-layout compatibility (§7). Pass 2 reproduced the exact vermagic from the Merlin tree with `LOCALVERSION=-5.02L.07` [V].
- **Config source [V]:** built with `CONFIG_IKCONFIG`/`IKCONFIG_PROC`. Extracted from `work/vmlinux.bin` (IKCFG_ST @ 5,870,056) to `work/os/kernel.config` (88,613 bytes). `CONFIG_KALLSYMS=y` (not `_ALL`, so data symbols are absent from the recovered symbol table).
- **Kernel cmdline [V]** (from `ubi/fs_filesystem1/data/kernel.log`):
  ```
  mtdparts=brcmnand.0:128k(nvram),640k(cfe),20096k(boot),205312k(ubi),63616k(conf),56832k(log),-(data)
  ubi.mtd=ubi,0 part_main=ubi part_boot=boot image_ubivol=operational
  board_type=00030103 root=mtd:rootfs init=/etc/preinit ro rootfstype=squashfs
  console=ttyS0,115200 hhtest=1 rootfs_offset=0x4f7000 rootfs_size=0x252d000
  coherent_pool=4M cpuidle_sysfs_switch pci=pcie_bus_safe rootwait cma=0M
  ```
  Note `console=ttyS0,115200`, `init=/etc/preinit`, `cma=0M` (`CONFIG_CMA` is not set at all).
- **Memory reservations [V]** (`work/devicetree.dts`, kernel.log): the DTB memory node says 512 MB but the bootloader rewrites it; the running kernel sees **1 GiB** (`Memory: 914000K/941052K available`; see 01 §1). Reserved-memory `dhd0/dhd1/dhd2` = 0xb00000 (11 MiB) each. *(Pass 2: refined)* These are the **Runner DHD-offload flow-ring pools**: `dhd_runner_flowmgr_init: dhd0_rnr bootmem addr<…> size<11534336>`. They are only needed while Runner offload is enabled (06 §2). `rdp1` 64 MB and `rdp2` 8 MB serve the Runner/RDP packet processor. Dongle RAM is 6 MB (`DHD: dongle ram size is set to 6291456`).
- **Built-in vs modular [V]:** core SoC bring-up is built in: PCIe RC `bcm963xx_pcie_probe`, NAND, serial, and the Broadcom BSP (`board_ioctl`, `kerSysGetMacAddress`, `pmc_init`, `PowerOnDevice`, `bcm_otp_get_row`, `bcm_ubus_config`, `bcm_misc_hw_init`, `bcm_gpio_set_data`, `bcm_led_driver_set`, all `T` symbols in `work/os/vmlinux.elf`). All Broadcom accelerators, WLAN, PON, netfilter, USB and Sagemcom drivers are **modules**, loaded in **several phases** (§2c), not only by S11.

### 1a. Module licences: exact counts [V]

143 `.ko`: **137** flat in `lib/modules/4.1.52-5.02L.07/` and **6** in `usr/lib/modules/4.1.52-5.02L.07/openvswitch/` (`openvswitch`, `vport-geneve/-gre/-lisp/-stt/-vxlan`). There are no `kernel/drivers/...` subdirectories. The `libata`/`ahci` paths in `bcm-base-drivers.sh` point at files that do not exist. Licence = `modinfo -F license` (script `OS_modinv.sh`, output `work/os/pass2/modinv.tsv`):

| Licence class | Modules | Exact tags found |
|---|---|---|
| GPL-compatible | **116** | `GPL` ×114 (`rdpa_gpl` repeats the tag 27 times, `bdmf` and `rdpa_gpl_ext` twice), `GPL v2` ×1 (`fusb301`), `Dual BSD/GPL` ×1 (`drv_gpio`) |
| Proprietary | **23** | `Proprietary` ×22, `proprietary` ×1 (`chipinfo`) |
| GPL and additional rights | **2** | `dhd`, `hnd` |
| Sagemcom | **2** | `Sagem Communications` (`hard_watchdog`), `SAGEMCOM` (`sgcnet`) |
| **Total** | **143** | |

**The 23 proprietary modules, by function [V]:**
- Ethernet/Runner path: `bcmlibs`, `bcm_pondrv` (a hard dependency of `rdpa` even for Ethernet WAN), `rdpa`. Acceleration and QoS: `pktflow`, `pktrunner`, `cmdlist`, `bcmvlan`, `bcm_ingqos` (`rdpa_cmd` depends on it).
- WLAN: `wlcsm`, `emf`, `igs` (dhd dependencies, see 06).
- PON: `pmd`, `gponstack`, `ngponstack`, `bcmgpon`, `bcmepon`.
- Voice: `slicslac`.
- Board data: `bsp_gpio_data`.
- Miscellaneous: `bcm_license`, `bcm_spdsvc`, `bcm_tcpspdtest`, `chipinfo`, `pwrmngtd`.

All 143 modules, with dependencies and the script that loads each one, are in **Appendix A**. 133 are loaded by some init script or daemon. **10 are never loaded** by any script: `ehci-pci ohci-pci xhci-pci sch_mqprio sch_qfq sch_sfb vport-geneve vport-lisp vport-stt vport-vxlan`.

### 1b. Config highlights for OpenWrt [V] (`work/os/kernel.config`)

| Area | Stock setting | Consequence for OpenWrt userspace |
|---|---|---|
| ABI | `ARM64=y`, `COMPAT=y`, `ARM64_4K_PAGES=y`, `ARM64_VA_BITS=39`, `SMP`, `NR_CPUS=4`, `PREEMPT=y`, `HZ=100`, `HIGH_RES_TIMERS` not set | Native aarch64 (OpenWrt `aarch64_cortex-a53`) and 32-bit ARM EL0 both run. Stock userspace is 32-bit (§5) |
| Namespaces | `NAMESPACES` **not set** (no UTS/IPC/PID/NET/USER ns) | No procd `ujail` isolation, no netifd netns, no containers (Docker/LXC/podman) |
| cgroups | `CGROUPS` **not set** | procd's `cgroup2` mount fails silently; no per-service cgroup limits |
| seccomp | `SECCOMP` **not set** | procd seccomp/`utrace` unavailable (optional packages) |
| nftables | `NF_TABLES` **not set** | **fw4 impossible.** Building the 4.1 nf_tables modules would not fix it: fw4's `table inet fw4` NAT chains need kernel ≥ 5.2 (§8) |
| iptables | `NETFILTER_XTABLES=y`, `IP_NF_IPTABLES=y`, `IP6_NF_IPTABLES=m`, conntrack/NAT/most `xt_*` `=m`; **`NETFILTER_XT_MATCH_COMMENT` not set**; `IP_NF_RAW` not set (but `ip6table_raw=m`); `IP6_NF_NAT` not set; `IP_SET` not set; `XT_MATCH_CONNMARK`/`IPRANGE`/`PHYSDEV`/`RECENT`/`TIME`/`HASHLIMIT`/`OWNER` not set; `NF_CONNTRACK_ZONES`/`LABELS`/`EVENTS`/`MARK` `=y`; `XT_TARGET_CT=m` | fw3 works only after you add `xt_comment.ko` (built, §7). No NAT66, no ipset; the raw table is missing for IPv4 (fw3 skips tables it cannot open) |
| Bridging / L2 | `BRIDGE=y`, `BRIDGE_NETFILTER=y`, `BRIDGE_VLAN_FILTERING` **not set**, `VLAN_8021Q=y`, `MACVLAN`/`VETH`/`TUN`/`DUMMY`/`BONDING`/`IFB` not set, `PPP`/`PPPOE=y`, `L2TP` not set, `IPV6=y`, `IPV6_SIT(+6RD)`/`IPV6_TUNNEL=y`, `NET_IPGRE=m`, `XFRM_USER`/`NET_KEY=y` | No netifd `bridge-vlan` (DSA-style) config: use VLAN subinterfaces. No OpenVPN (tun) or WireGuard unless you build the modules (§7) |
| QoS | `NET_SCHED=y`, `HTB`/`TBF`/`INGRESS=y`, `FQ_CODEL`/`FQ` not set, `NET_ACT_MIRRED` not set, `BPF_SYSCALL`/`BPF_JIT` not set | No default fq_codel, no SQM ingress shaping (IFB/mirred), no qosify (eBPF). `sch_fq_codel`/`ifb`/`act_mirred` can be built from the 4.1 tree [I] |
| Filesystems | `SQUASHFS=y` (+`XZ`, `ZLIB`; no `LZO`/`LZ4`/`XATTR`), `UBIFS_FS=y` (`LZO`, `ZLIB`), `MTD_UBI=y`, `MTD_UBI_BLOCK=y`, `UBI_FASTMAP` not set, `OVERLAY_FS=y`, `JFFS2_FS=y`, `EXT4_FS=y`, `VFAT_FS=y`, `F2FS_FS` not set, `DEVTMPFS(+_MOUNT)=y` | An OpenWrt squashfs-xz rootfs plus a UBIFS/JFFS2 overlay with upstream overlayfs `workdir` semantics all work. **`SHMEM` not set**, so `tmpfs` is the ramfs-backed "tiny-shmem" (`mm/shmem.c` `!CONFIG_SHMEM` branch registers `"tmpfs"` with `ramfs_mount`, and ramfs ignores `size=`) [V source]: `/tmp` has **no size limit** |
| Syscalls | `EPOLL`/`TIMERFD`/`EVENTFD`/`INOTIFY_USER=y`; `SIGNALFD`, `FHANDLE`, `AIO`, `FANOTIFY`, `CHECKPOINT_RESTORE`, `INET_DIAG`, `NETLINK_DIAG` not set | libubox `uloop` (epoll + timerfd) is fine. `ss` and other diag tools fail |
| Modules / hardening | `MODULES`/`MODULE_UNLOAD=y`, `MODVERSIONS` and `MODULE_SIG` off, `CC_STACKPROTECTOR_NONE=y`, `STRICT_DEVMEM=y` | Out-of-tree modules load if vermagic and layouts match |
| Wireless | `CFG80211=m` (Broadcom-patched with backports, see 06), `MAC80211` **not set**, `RFKILL` not set | No mac80211 drivers (USB WiFi sticks, mt76/ath) without building mac80211 for 4.1 [I] |
| Misc | `FW_LOADER=y`, `UEVENT_HELPER_PATH="/sbin/hotplug"`, `PCI_MSI=y`, `HW_RANDOM_IPROC_RNG200=y`, `CRYPTO_AES/SHA256/CCM/CMAC/CTR=y`, `GCM` not set, `ARM64_CRYPTO` not set, `PRINTK_TIME` off, `FTRACE`/`PERF_EVENTS` off, `CMA` not set | No ARMv8 CE crypto acceleration |

---

## 2. Boot / init sequence

Init model is **classic (pre-procd) OpenWrt**: BusyBox `init` → `/etc/inittab` → `/etc/init.d/rcS` → `/etc/rc.d/S*`. **No procd, ubus, ubusd or uci binaries exist** (`sbin/procd`, `sbin/ubusd`, `bin/ubus`, `sbin/uci` are all absent). `config_*` shell helpers in `/lib/functions.sh` give uci-like parsing. [V]

### 2a. preinit (`/etc/preinit` + `/lib/preinit/*`) [V]
`init=/etc/preinit` runs first. Sources `/lib/functions.sh`, `/lib/functions/boot.sh`, registers hook lists (`preinit_essential`, `preinit_main`, `failsafe`, `initramfs`, `preinit_mount_root`), sources all `/lib/preinit/*`, runs hooks. Ordered hook files:

| File | Role (essential?) |
|---|---|
| 05_mount_skip, 10_check_for_mtd, 10_essential_fs | mount proc/sys/tmp — **essential** |
| 10_indicate_preinit / _failsafe | LED/screen status |
| 20_check_jffs2_ready, 20_device_fs_mount, 30_device_fs_daemons | mount device/overlay fs |
| 30_failsafe_wait, 40_run_failsafe_hook, 99_10_failsafe_login | failsafe net (192.168.1.1) + shell |
| 40_init_shm, 40_mount_devpts, 40_mount_jffs2, 41_merge_overlay_hooks | shm/pts/jffs2 overlay |
| 50_choose_console, 90_init_console | console = ttyS0 |
| 70_pivot_jffs2_root (`fopivot /overlay /rom`) | **overlay pivot — writable layer over squashfs rom** |
| 80_mount_root, 90_mount_no_jffs2 | mount real root |
| 90_restore_config | if `/sysupgrade.tgz` present, untar over `/` (config restore) |
| 99_10_run_init | exec `/sbin/init` |

### 2b. init.d / rc.d order [V] (`ls etc/rc.d`, script bodies read in pass 2)
`inittab`: `::sysinit:/etc/init.d/rcS S boot`. `rcS` runs `/etc/rc.d/S*` in numeric order. Columns E/W/P/S say whether the step is needed for **E**thernet-only, **+W**iFi, **+P**ON, **+S**creen/buttons (R = required, O = optional, – = not needed, ? = unknown):

| Order | Script | What it actually does (pass 2) | E | W | P | S |
|---|---|---|---|---|---|---|
| S05 | user-filesystems | mount user volumes | R | R | R | R |
| S06 | mkdev | /dev nodes | R | R | R | R |
| S07 | ubibootfs | mount UBI boot vol | R | R | R | R |
| S08 | registertemperaturesensor | `echo tmp102 0x48 > /sys/class/i2c-dev/i2c-0/device/new_device` (TMP102 driver is built in) | O | O | O | O |
| **S10** | **boot** | dirs, `date 2013-01-01`, debugfs, **`load_modules /etc/modules.d/*`** (first module phase, §2c), `cat loading.raw > /dev/fb0` | R | R | R | R |
| S10 | firewall-app | Sagemcom firewall daemon (also `insmod`s conntrack helpers) | – | – | – | – |
| **S11** | **bcmdrivers → `/etc/bcm-base-drivers.sh start`** | Broadcom accel + WLAN modules (§2d) | R | R | R | – |
| S12 | check_bad_reboots, ubidatafs | data vol + reboot-loop guard | O | O | O | O |
| S15 | wanconf | `/usr/bin/wanconf`: per PSP `RdpaWanType` it insmods `pmd`, `gponstack`/`ngponstack`, `bcmgpon` or `bcmepon` (strings `insmod %s %s`, `*.ko`), and tries `extra/wantypedet.ko` (absent). Converts eth4 to WAN for GBE (02) | O¹ | – | R | – |
| S16 | check_restore_default | factory-reset check | – | – | – | – |
| S19 | portmap | RPC | – | – | – | – |
| S20 | qosa | **not QoS**: copies `m02099_firmware.bin` from `calib` to `/lib/firmware`, loads optics drivers `m02180`/`m02099` in a WAN-type-dependent order, links `/dev/laser_dev`, `serdesctrl`/`bs` tweaks | – | – | R | – |
| S25 | gui-firmware | screen/GUI assets | – | – | – | O |
| S29 | register_mcu_button_reboot | `insmod mcu-core`, `mcu-i2c`, `mcu-keys no_thermal_alert=1`; `echo f5689-mcu 0x78 > …/i2c-0/…/new_device` | – | – | – | R |
| S30 | mcu_firmware_update | MCU firmware update | – | – | – | O |
| S39 | usb | only mounts `usbfs`; USB modules are loaded by `/etc/init.d/usb_mod`, called from hg6d's `libusb-smu.so` (no rc.d link) | – | – | – | – |
| **S40** | **network** | bring up bridges/interfaces | R | R | R | – |
| S40 | syslog, voice | logging, VoIP | – | – | – | – |
| S45 | ismd | IGMP/multicast snooping | O | O | O | – |
| **S48** | **wificountrycode** | `wlctl -i wl{0,1,2} country CA/0`, then `/etc/checkwifi.sh &` | – | R | – | – |
| S49 | sysctl | sysctl.conf | R | R | R | – |
| S50 | cron, hg6d, telnet | hg6d middleware, cron, (gated) telnetd | – | – | – | – |
| S55 | firewall-utils | firewall helpers | – | – | – | – |
| S60 | isc-dhcp | DHCP server | – | – | – | – |
| S63 | notifications_manager | event notifier | – | – | – | – |
| S70 | dropbear | host keys only (see §4, unchanged) | – | – | – | – |
| **S90** | **hostap** | `hostapd -g /var/run/hostapd/global -B -P …` + `wpa_supplicant -g … -B -s` | – | R | – | – |
| S90 | rdpaconf | `bs /b/c iptv {lookup_method=group_ip_src_ip}`, `fc config --accel-mode 0 --tcp-ack-mflows 1 --mcast-learn 0`, `bs /d bbh s pkt_size0 wan 48 1536` | O | O | O | – |
| S95 | done | boot-complete marker/LED | – | – | – | O |
| S99 | samknows_ispmon, tr_surveil | SamKnows probe, TR-069 surveil | – | – | – | – |

¹ For Ethernet WAN on eth4 you can set the PSP once and skip wanconf (02 §9). K-scripts (shutdown): K19portmap, K93notifications_manager, K98boot, K99samknows_ispmon, K99umount.

### 2c. Module load phases (pass 2) [V]

| Phase | Loader | Modules |
|---|---|---|
| 1 | **S10 `boot` → `load_modules /etc/modules.d/*`** (`/lib/functions.sh`: `sed 's/^[^#]/insmod &/' … \| ash`) | 32 files, in this order: `bcmlibs`; `cfg80211`; `libcrc32c` (`crc32c` absent); `nf_conntrack nf_defrag_ipv4 nf_conntrack_ipv4 xt_state`; **`bsp_gpio_lkm bsp_gpio_data`**; **`hard_watchdog`**; **`drv_gpio`**; `cifs-server`; iptables core/NAT/helpers/ip6tables/arptables/`nf_conntrack_netlink`/`nfnetlink_queue`; `xt_blog`, `xt_SKIPLOG` (Broadcom); **`bcm_ingqos`**; `usblp`, `usb-storage` (`usb-uas` and `ethdriver_lkm` absent); `sgcnet` (Sagemcom IGMP statistics ioctl); **`ssd1322`**; `slicslac dsphal` (voice); `nf_conntrack_rtsp ports=554,5004`; `sch_hfsc sch_multiq cls_u32 sch_red sch_sfq sch_teql` (the other 25 names in `99-sched` are absent or built in) |
| 2 | **S11 `bcm-base-drivers.sh`** | see §2d |
| 3 | S15 `wanconf` | `pmd`, `gponstack`, `ngponstack`, `bcmgpon`, `bcmepon` depending on WAN type. `gponstack.ko` does not appear as its own string; it is probably the tail-merged suffix of `ngponstack.ko` [I] |
| 4 | S20 `qosa` | `m02180`, `m02099` (order depends on GPON or XGS) |
| 5 | S29 | `mcu-core`, `mcu-i2c`, `mcu-keys no_thermal_alert=1` |
| 6 | hg6d plugins (runtime) | `usb_mod`: `ehci-hcd ehci-platform ohci-hcd ohci-platform xhci-hcd xhci-plat-hcd uhci-hcd bcm_usb uas fusb301`, then `echo fusb301 0x25 > …/i2c-0/…/new_device`; `libstorage-smu.so`: `insmod cifs-server` |

Ordering subtleties [V]: `bcmlibs` and `cfg80211` are loaded in phase 1, so their S11 `insmod`s fail harmlessly as duplicates. `rdpa_cmd` (S11) depends on **`bcm_ingqos`, which only phase 1 loads**. `openvswitch` (S11) depends on the conntrack/NAT modules from phase 1. A replay that skips `modules.d` breaks both.

### 2d. Full S11 `/etc/bcm-base-drivers.sh` sequence with parameters [V]

Same E/W/P/S columns as §2b. "absent" = the `.ko` does not exist in this rootfs (`insmod` fails silently).

| # | Step (exact) | Licence | E | W | P | S | Notes |
|---|---|---|---|---|---|---|---|
| 1 | `insmod bcmlibs.ko` | Prop | R | R | R | – | duplicate of phase 1; needed by `rdpa`, `pktflow`, `wfd`, `dhd` |
| 2 | `insmod wlcsm.ko` | Prop | ? | R | ? | – | only `hnd` and `dhd` list it in `depends` [V]; Ethernet runtime need [U] (02 §9) |
| 3 | `insmod bdmf.ko` | GPL | R | R | R | – | |
| 4 | `insmod opticaldet.ko` | GPL | R | R | R | – | dependency of `bcm_pondrv` |
| 5 | `insmod bcm_pondrv.ko` | Prop | R | R | R | – | hard dependency of `rdpa`, even for Ethernet WAN |
| 6 | `insmod rdpa_gpl.ko` | GPL | R | R | R | – | |
| 7 | `insmod rdpa_gpl_ext.ko` | GPL | R | R | R | – | dependency of `rdpa_cmd`, `bcmgpon`, `bcmepon` |
| 8 | `insmod rdpa.ko` | Prop | R | R | R | – | Runner data path |
| 9 | `insmod rdpa_usr.ko` | GPL | R | R | R | – | `/dev/rdpa_user` for `bdmf_shell`/`bs` |
| 10 | `insmod rdpa_mw.ko` | GPL | R | R | R | – | `rdpa_init_system`, latches the WAN type (02 §5.1) |
| 11 | mount `calib` mtdblock read-only at `/mnt/nvram`, `cp bcm_license.txt /etc/licenses/`, umount | – | ? | ? | ? | – | |
| 12 | `insmod bcmbrfp.ko` | GPL | O | O | O | – | bridge fast path (accel) |
| 13 | `insmod bcm_license.ko`; `cat /etc/licenses/bcm_license.txt > /proc/driver/license` | Prop | ? | ? | ? | – | comment says "BCM ovs license"; gates licensed features [U] |
| 14 | `insmod bcm_spdsvc.ko`, `insmod bcm_tcpspdtest.ko`; `cat … > /proc/spdsvc/license` | Prop | – | – | – | – | speed-test service |
| 15 | `insmod chipinfo.ko` | Prop | ? | ? | ? | – | [U] |
| 16 | `insmod pktflow.ko` | Prop | O² | O | O | – | flow cache; `pktrunner` depends on it |
| 17 | `insmod bcm_enet.ko` | GPL | R | R | R | – | Ethernet driver |
| 18 | `insmod cmdlist.ko`, `insmod pktrunner.ko` | Prop | O | O | O | – | HW flow acceleration |
| 19 | `insmod bcmmcast.ko` | GPL | O | R | O | – | dependency of `wfd`, `emf`, `igs`, `dhd` |
| 20 | `libata`, `libahci`, `ahci`, `bcm_sata`, `libahci_platform`, `ahci_platform` | absent | – | – | – | – | no SATA |
| 21 | `sh /bin/load_kernel_nvram.setting` | – | – | R | – | – | loads WLAN calibration into `/proc/nvram` (06 §1e) |
| 22 | `nvram set {1,2,3}:ccode=CA`, `{1,2,3}:regrev=0`, `regrev=0`; `nvram commit`; `nvram kcommit` | – | – | R | – | – | country/regrev |
| 23 | `insmod bcm_pcie_hcd.ko` | absent | – | – | – | – | PCIe RC is built in |
| 24 | `insmod wfd.ko` | GPL | – | R | – | – | dhd dependency (Runner WLAN forwarding) |
| 25 | `insmod hnd.ko`, `insmod emf.ko`, `insmod igs.ko` | GPL+AR / Prop / Prop | – | R | – | – | dhd dependencies |
| 26 | `FW_PATH=/etc/wlan/dhd/`, or `/etc/wlan/dhd/mfg/` if `/proc/nvram/wl_nand_manufacturer` ∈ {2,3,6,7} | – | – | R | – | – | |
| 27 | `insmod cfg80211.ko` | GPL | – | R | – | – | duplicate of phase 1 |
| 28 | **`insmod dhd.ko dhd_dpc_prio=5 iface_name=wl dhd_console_ms=0 firmware_path=$FW_PATH`** | GPL+AR | – | R | – | – | creates `wl0..wl2` |
| 29 | `insmod wl.ko` | absent | – | – | – | – | SoftMAC driver not shipped |
| 30 | `insmod bcmvlan.ko` | Prop | O² | O | O | – | Broadcom VLAN (`vlanctl`) |
| 31 | `insmod pwrmngtd.ko` | Prop | ? | ? | ? | – | power management [U] |
| 32 | `insmod i2s.ko` | absent | – | – | – | – | |
| 33 | `insmod rdpa_cmd.ko` | GPL | R² | R | R | – | needs `bcm_ingqos` from phase 1 |
| 34 | `/etc/rdpa_init.sh`: `ifconfig eth0 down`; `cat /dev/rgs_logger &`; `bdmf_shell -c init` → `/var/bdmf_sh_id` | – | R | R | R | – | |
| 35 | `insmod gre.ko`, `ip_gre.ko`, `nf_nat_ipv6.ko` (dup), `openvswitch.ko`, `vport-gre.ko` (each behind `test -e`) | GPL | – | – | – | – | OpenSync/Plume only |
| 36 | `echo 1 > /sys/devices/platform/bcm_pka/enable` | – | O | O | O | – | PKA crypto engine |
| 37 | `wlaffinity auto` | – | – | O | – | – | WLAN IRQ/thread pinning |

² In doc 02 §9's minimum Ethernet set [I, untested].

**Screen/buttons** do not need S11. They need phase 1's `bsp_gpio_lkm`/`bsp_gpio_data` (BSP board data; the module also exposes `bsp_reset_gpio_lan`/`_usb`/`_voice` resets, so an Ethernet role is [U]), `drv_gpio` (LED and button driver), `ssd1322` (`/dev/fb0`) and `hard_watchdog` (pacifies the BCM963xx watchdog timer; whether the hardware watchdog is armed before Linux is [U], see 04), plus S29 (MCU keys/thermal over I2C 0x78). Details are in 04.

---

## 3. Middleware (Sagemcom hg6d / XMO) & services

- **hg6d [V]:** launched by `S50hg6d` via `start-stop-daemon -S -b -x hg6d_start`; `hg6d_start` = `ulimit -c unlimited; ulimit -s 2048; hg6d -d || /usr/bin/reboot.sh dev`. `BOOT_HG6D_ENABLE` gate in `/etc/default/hg6d` (empty here → runs). hg6d is the central **XMO** data-model engine (Sagemcom's TR-181-like object model): it owns config, spawns/monitors service tasks, and exposes the model to the web UI, TR-069 and Plume.
- **Data-model plugins [V]:** `usr/lib/xmo/plugins/*.so` implement model objects. e.g. `libuser-impl-smu.so` implements UserAccounts **and spawns the SSH daemon** (string `dropbear -F -j -k -U %s -p [%s]:%d -P %s.pid.%s.%s`, tag `[SSH-TASK]`, "dropbear process should be running. restart it now!"). This is why nothing in `/etc` launches dropbear — hg6d does, driven by the UserAccounts model.
- **Config storage [V]:**
  - Factory/data-model XML in UBI `conf` volume: `ubi/fs_filesystem1/conf/backup-cfg-*.xml` (UserAccounts, ManagementServer, Services_Plume, WiFi, …) — each versioned (e.g. `-12.xml`).
  - `start-cfg.xml`, `hg6d.conf` in `/etc` seed defaults.
  - `psi` / `scratchpad` in UBI `fs_overlay` hold persistent runtime state (`.kernel_nvram.setting`, `newkey`/`newkey.cert` device certs).
  - Logs in `conf`/`log` volumes: `data/kernel.log*`, `operator_log_*`.
- **TR-069 (ManagementServer) [V]** (`backup-cfg-ManagementServer-8.xml`):
  - **ACS URL: `https://cwmp.acs.bell.ca`**
  - ACS auth user `8020DA-<serial>` (OUI 8020DA = Sagemcom), per-device password (**sensitive**, §4). Local CWMP port 58603; CA cert `/etc/VerisignClass3.cer`; SSL CN verification on. Vendor namespace `tr181bell` → `http://sagemcom.com/tr181bell-data`. Active notifications for WiFiPODS (Plume), cellular failover, PPP status.
  - TR-069 daemon `usr/bin/tr69d`; `S99tr_surveil` watchdogs it.
- **Plume / OpenSync [V]:** `etc/init.d/managers` (© "Plume Design Inc"), `/usr/opensync/` with managers `dm cm nm wm sm qm bm fcm fsm lm om xm` (diag/connection/network/wifi/stats/queue/band-steering/… managers) plus `dhcpd-opensync`, OVS (`ovs-vswitchd`, `ovsdb-server`, `ovsh`). OpenSync bridges the device into Bell's Plume cloud (HomePass/WiFiPODS mesh, band steering, adaptive WiFi) and uses OpenvSwitch (`usr/lib/modules/.../openvswitch/`) for the data path. Mesh/EasyMesh is **Plume-cloud driven**, not hostapd/wpa mesh.
- **Web UI [V]:** `lighttpd` + `/www`; `bell-screend` drives the front LCD. HTTPS admin on 443 / 50000 / 50001 (per UserAccounts RemoteAccess ports).
- **Firmware upgrade (high level) [V]:** TR-069 Download or local GUI pushes an image; `gui-firmware`/`S25` stages assets; dual-bank UBI (`operational`/other) + GSDF detail → boot/flash agent. `sysupgrade.tgz` + preinit `90_restore_config` is the OpenWrt-style config carry-over. `lib/upgrade/keep.d/` lists files preserved across upgrade (`dropbear`, `base-files`).

---

## 5. Userspace ABI / toolchain

- **Userspace is 32-bit ARM, not aarch64 [V]** *(pass 2 correction)*. `file`: `bin/busybox`, `usr/bin/hg6d`, `lib/libc-2.27.so` and `usr/sbin/hostapd` are `ELF 32-bit LSB … ARM, EABI5`, interpreter `/lib/ld-linux.so.3`. `readelf -A bin/busybox`: `Tag_CPU_arch: v7`, no `Tag_ABI_VFP_args`, so this is the soft-float calling convention (armel), not armhf. Of the ELF files in the rootfs, **698 are 32-bit ARM**. The only 64-bit ones are 2 unused copies of `ld-linux-aarch64.so.1`, and there is one stray MIPS binary, `usr/bin/ookla-client`. The kernel is arm64 with `CONFIG_COMPAT=y`, so all stock userspace runs through the compat syscall layer, including the 32-bit ioctls of `wl`, `dhd`, `bdmf_shell` and `ethctl`.
- **libc [V]:** **glibc 2.27** (`lib/libc-2.27.so`, `ld-2.27.so`). NOT musl/uClibc. Stock binaries need these 32-bit glibc libraries.
- **BusyBox [V]:** `v1.30.1 (2022-04-15)`.
- **Toolchain:** the kernel was built with gcc 5.5.0 (crosstool-NG 1.23.0) [V, banner]. The userspace compiler is [U], because `.comment` is stripped.
- **OpenWrt heritage [V]:** preinit/boot_hook framework, `/etc/rc.common`, `/lib/functions.sh` config_* helpers and `load_modules`, `/lib/upgrade/keep.d`, failsafe mode. Era: pre-procd, about 2016 ("Chaos Calmer") [I]. There is **no procd/ubus/uci**; services are managed by BusyBox init plus Sagemcom hg6d.
- **Implication for OpenWrt [I]:** an OpenWrt userspace can be native **aarch64** (`aarch64_cortex-a53`, the same package architecture as OpenWrt's `bcm4908` target, whose B53 cores match; `target/linux/bcm4908/Makefile`: `ARCH:=aarch64`, `CPU_TYPE:=cortex-a53` on openwrt-24.10 and -25.12 [V]). To keep using stock Broadcom tools (`bdmf_shell`, `wl`, `dhd`, `ethctl`, `fc`, `serdesctrl`), also ship their 32-bit glibc 2.27 libraries in a side directory (02 §9).

---

## 7. Kernel rebuild feasibility from the Broadcom 4.1 GPL source (pass 2)

Source: `$P/work/os/merlin/release/src-rt-5.02L.07p2axhnd/` plus `$P/work/eth/ref/merlin/…`. Both are sparse checkouts of RMerl/asuswrt-merlin.ng, a single-commit clone at HEAD `7048222b4` (2026-10-31). Build copy: `$P/work/os/kbuild/kernel/linux-4.1/`.

### 7a. Is BCM6858/arm64 there? Partly [V]

| Piece | In the Merlin tree? | Evidence |
|---|---|---|
| Core kernel 4.1.52 with Broadcom `BCM_KF_*` patches | **Yes** | `kernel/linux-4.1/Makefile` `VERSION=4 PATCHLEVEL=1 SUBLEVEL=52` |
| `CONFIG_BCM96858` | **Yes** | `Kconfig.bcm`: `config BCM96858 bool "6858 GPON/EPON" depends on BCM_KF_ARM64_BCM963XX && ARM64` |
| 6858 device trees | **Yes** (Broadcom reference boards) | `kernel/dts/6858/96858.dts`, `949508EAPAX.dts` |
| 6858 register maps, lport/phys, boardparms, PMC headers | **Yes** | `shared/opensource/include/bcm963xx/6858_map_part.h`, `shared/opensource/drv/lport/*`, `include/pmc/*` |
| XRDP_CFE BCM6858_B0 minimal Runner datapath | **Yes** | `rdp/projects/XRDP_CFE*/…/BCM6858` (see 02, 11) |
| **Board driver** (`kerSysGetMacAddress`, `board_ioctl`, flash/NVRAM) | **No source** | `bcmdrivers/opensource/char/board/bcm963xx/impl1/` contains only a `Makefile` |
| **PCIe RC driver** (`bcm963xx_pcie_probe`, needed for WiFi) | **No source** | `bcmdrivers/opensource/bus/pci/host/impl1/` contains a `Makefile` and a DT binding `.txt` |
| **arm64 platform setup, PMC, DT post-init, console, OTP, IRQ** | **No source** | `char/plat-bcm/impl1/` (Makefile + one ARMv7 `.S`), `shared/opensource/pmc/impl1/` (Makefile + a 63158 image) |
| wlcsm / wfd | wlcsm: none. wfd: headers only in 5.02L.07 | source for wfd only in `src-rt-5.02axhnd.675x` and `5.04*` |

ASUS ships the missing BSP pieces as **prebuilt objects** for its own boards: `router-sysdep.{rt-ax86u,gt-axe11000,rt-ax68u,rt-ac68u_v4}/hnd_extra/prebuilt/` (about 80 `.o` each: `board*.o`, `bcm_arm64_setup.o`, `bcm_pcie_hcd.o`, `pmc_drv.o`, `bcm_dt.o`, `bcm63xx_cons.o`, `bcm63xx_flash.o`, `otp.o`, `bcm_enet.o`, `bdmf.o`, `dhd.o`, `hnd.o`, `emf.o`, `igs.o`, …). The RT-AX86U and GT-AXE11000 are BCM4908 boards, and `board.o` contains "4908" strings but no "6858" [V]. Chip selection in this BSP is compile-time (`#if defined(CONFIG_BCM9xxxx)`), so these objects cannot be relinked into a 6858 kernel [I, high confidence].

**The Merlin source is also not the stock source [V].** Running the stock `.config` through Merlin's Kconfig drops **45** stock symbols and adds 4 (`work/os/pass2/config_diff.txt`):
- Sagemcom additions: `SGC_RUNNER_DHD_POOL*`, `SGC_RUNNER_TM_SIZE`, `SGC_WLAN_*`, `MTD_SECURE_UPDATE_*`, `OVS_OPENSYNC_ACTION`, `BR_IGMP_SNOOP`, `BR_MLD_SNOOP`, `IGMP_STATS`, `CUSTOM_DSCP_TO_PRIO_MAPPING`, `EXFAT_*`, `USB_CC_FUSB301*`.
- Broadcom `BCM_KF_ADD_EMERGENCY_HEADER_SIP`, `BCM_KF_CONSOLE_BAUD`, `BCM_KF_DISABLE_SIP_ALG`, `BCM_COMMON_CLK*`, `BCM_OTP_BRCM_BTRM_BOOT_ENABLE`.
- `BCM_LIBS*`, `BCM_MCAST*`, `BCM_PON_DRV*`, `BCM_SMTC*`. These probably come from the bcmdrivers `Kconfig.autogen`, which is empty here [I].
- Merlin-only additions: `BCM_KF_XT_MATCH_LAYER7`, `BCM_KF_XT_TARGET_DC`, `NET_UDP_TUNNEL=m`, `WIREGUARD=m`.

**Verdict:** a **bootable** BCM6858 kernel cannot be built from Merlin, because the BSP source is missing, and nothing else public is closer (no Sagemcom/Bell GPL drop, doc 10 §6). Realistic options:
1. Keep the **stock 4.1.52 binary kernel** and its modules, and add out-of-tree modules (below). This is the recommended homelab route.
2. Mainline Linux with new 6858 drivers (doc 10 §3, doc 02 §9, doc 11).
3. Ask Sagemcom for the GPL source [U].

### 7b. Module ABI compatibility with the stock kernel: tested [V]

Build command (from the kbuild copy):
```
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- BUILD_NAME=HH4000 LOCALVERSION=-5.02L.07 \
     HOSTCFLAGS=-fcommon KCFLAGS="-Wno-error -fno-pic -DHH4000_STOCK_ABI" KBUILD_MODPOST_WARN=1 M=<dir> modules
```
`BUILD_NAME` is needed because the ASUS Makefile adds `-D$(MODEL)`. `HOSTCFLAGS=-fcommon` lets 4.1's `dtc` link with gcc 15. Compiler: Ubuntu `aarch64-linux-gnu-gcc` 15.2. Struct layouts came from kbuild `DEFINE()` probes (`OS_layout_probe*.c` → `.s`, results in `work/os/pass2/layout_merlin_full.txt`) and were compared with constants in the stock code:

| Item | Merlin build | Stock evidence (`vmlinux.elf` / stock `.ko`) | Match |
|---|---|---|---|
| `sizeof(struct sk_buff)` / `sk_buff_fclones` | 512 / 1088 | `skb_init`: `mov x1,#0x200`, `mov x1,#0x440` | ✓ |
| `skb->len/data_len/tail/end/head` | 88/312/448/452/456 | `skb_put`: `[x0,#88]`, `[x0,#312]`, `[x0,#448]`, `[x0,#452]`, `[x0,#456]` | ✓ |
| `sizeof(struct net_device)` | 1856 | `alloc_netdev_mqs`: `add x19,x19,#0x75f` (1856+31); `netdev_rx_handler_register`: `#0x740` | ✓ |
| `net_device->netdev_ops/mtu` | 680/720 | `dev_set_mtu`: `[x19,#680]`, `[x0,#720]` | ✓ |
| `sizeof(struct task_struct)` | 2544 | `fork_init`: `mov x1,#0x9f0` | ✓ |
| `sizeof(struct module)` | 0x1d8 | `.gnu.linkonce.this_module` = 0x1d8 in stock `xt_mark`, `xt_limit`, `nf_conntrack_ftp`, `sch_sfq` | ✓ |
| `sizeof(struct nf_conn)` | **392** as shipped, **384** with the patch | `nf_conntrack.ko`: `kmem_cache_create(…, 0x180)`. `ct->timeout` @240 (timer `.data` store at #272) and `__nfct_init_offset` @296 (16-byte memset) also match | ✗ → **✓ after patch** |
| vermagic | `4.1.52-5.02L.07 SMP preempt mod_unload aarch64` | identical on all 143 stock modules | ✓ |

- **The one layout difference found:** ASUS appended `biqos_cb_t cb;` (`typedef struct biqos_cb { u_int32_t skb_mark; }`, ASUS adaptive-QoS) to `struct nf_conn` in `include/net/netfilter/nf_conntrack.h`. The stock kernel does not have it. Pass 2 wrapped it in `#ifndef HH4000_STOCK_ABI` (`work/os/pass2/nf_conntrack_hh4000.patch`). Any module that touches `nf_conn` (conntrack, NAT, `xt_conntrack`, helpers) must be built with this flag [V].
- **Modules built:** `xt_comment.ko` (needed by fw3, §8), `xt_mark`, `xt_limit`, `nf_conntrack_ftp`, `sch_sfq` (`work/os/pass2/modbuild/`; `xt_comment.ko` also in `work/os/pass2/built-modules/`). `xt_comment.ko` imports only `xt_register_match`/`xt_unregister_match`, both exported by the stock vmlinux [V]. **Loading on the device has not been tested [U].**
- **WireGuard is possible [V build, I runtime].** Merlin's 4.1 tree contains `net/wireguard` (wireguard-linux-compat `1.0.20220627`), and it builds against the stock ABI (`work/os/pass2/wgbuild/wg/wireguard.ko`, 168 KB). The only imports the stock kernel and modules lack are the `udp_tunnel` helpers (`udp_sock_create4/6`, `setup_udp_tunnel_sock`, `udp_tunnel_xmit_skb`, `udp_tunnel6_xmit_skb`, `udp_tunnel_sock_release`), because stock has `NET_UDP_TUNNEL` off. Build `net/ipv4/udp_tunnel.c` and `net/ipv6/ip6_udp_tunnel.c` from the same tree as modules. Data symbols such as `jiffies` or `init_net` only look missing because `KALLSYMS_ALL` is off.
- **Caveats [I]:** comparing per-function offset immediates between gcc 5.5 (stock) and gcc 15 (rebuilt) objects is noisy, so only the structures in the table are verified. For anything beyond small netfilter, qdisc or tunnel modules, build with a gcc 5.x toolchain like stock (crosstool-NG 1.23). Avoid rebuilding bridge or IGMP code: the Sagemcom-only options `BR_IGMP_SNOOP`, `BR_MLD_SNOOP` and `IGMP_STATS` are absent from the Merlin source and may change those structs [U].

### 7c. Buildable additions vs hard limits on the stock kernel [I]
- **Buildable as modules (GPL, from the same tree):** `xt_comment` (needed by fw3), `iptable_raw`, `ip6table_nat` (NAT66), `xt_connmark`/`iprange`/`time`/`recent`/`physdev`/`hashlimit`, `ip_set*`, `tun` (OpenVPN), `wireguard` + `udp_tunnel`, `sch_fq_codel`, `ifb` + `act_mirred` (SQM shaping), `veth`/`macvlan`/`dummy`. The nf_tables modules would build too, but they do not help fw4 (§8).
- **Impossible without a new vmlinux:** namespaces, cgroups, seccomp, `SHMEM` (real tmpfs), `BRIDGE_VLAN_FILTERING` (bridge is built in), BPF syscall/JIT, `HIGH_RES_TIMERS`, and anything from kernels newer than 4.1 (inet-family NAT, flowtables, cake from mainline).

---

## 8. Newest OpenWrt userspace usable on this 4.1 kernel (pass 2)

Method: the OpenWrt package Makefiles of each release branch (`work/os/owrt/mk/<rel>-<pkg>.mk`, fetched from `github.com/openwrt/openwrt` branches `openwrt-19.07` … `openwrt-25.12`) give the pinned `PKG_SOURCE_VERSION`. At those commits, full clones of procd, libubox, ubus, netifd, fstools, firewall3 and firewall4 (`work/os/owrt/<pkg>/`) were scanned for kernel-feature use (`OS_owrt_kfeat.sh`, output `work/os/pass2/owrt_kfeat.txt`), and the hits were read in context.

| Release | OpenWrt kernel (armvirt/armsr) | musl | Default firewall | Kernel features used beyond 4.1 | On the stock 4.1 kernel |
|---|---|---|---|---|---|
| 19.07 | 4.14 | 1.1.24 | fw3 | none found | **usable** [I] |
| 21.02 | 5.4 | 1.1.24 | fw3 | procd: cgroup2 mount + service cgroups, `CLONE_NEWTIME` (jail only) | **usable**; cgroup and jail features inert |
| 22.03 | 5.10 | 1.2.3 | **fw4** (fw3 still packaged) | fw4: nf_tables + inet NAT (≥ 5.2); netifd: `IFLA_BR_VLAN_FILTERING` | **usable with fw3** |
| 23.05 | 5.15 | 1.2.4 | fw4 (fw3 at `4cd7d4f`, same as 22.03) | as 22.03 | **usable with fw3** |
| **24.10** | 6.6 | 1.2.5 | fw4 (fw3 at `1aef979`, 2024-10-18; `iptables-zz-legacy` 1.8.10 still packaged) | libubox `uloop` uses `timerfd_create` (Linux 2.6.25) | **usable with fw3: newest released branch checked in full** [V source, I runtime] |
| 25.12 | 6.12 | 1.2.5 | fw4 (fw3 at `3a65fde`, 2025-10; `iptables-zz-legacy` packaged; apk instead of opkg) | ubusd `getrandom()` (3.17), netifd `RTA_PREF` (4.1) | **probably usable with fw3** [I] |

Kernel versions are from `target/linux/{armvirt,armsr}/Makefile` `KERNEL_PATCHVER`; musl from `toolchain/musl/common.mk`; all fetched from the release branches on 2026-11-04 [V].

**Per-component findings [V source]:**
- **procd** (all releases): `initd/early.c` mounts proc, sysfs, `cgroup2` (21.02+), tmpfs `/dev` (`size=512K`), devpts and tmpfs `/tmp`, and **ignores every `mount()` return value**. `instance_add_cgroup()` returns early when `/sys/fs/cgroup/cgroup.subtree_control` is missing and only logs a warning. seccomp and ujail are used only by instances that ask for them (separate `procd-seccomp`/`procd-ujail` packages). So procd runs on 4.1 without namespaces, cgroups or seccomp. With `SHMEM` off, both tmpfs mounts become ramfs, which ignores `size=`.
- **libubox / ubus:** epoll, plus timerfd from 24.10 on, plus `getrandom` in the 25.12 ubusd. All of these exist on 4.1.
- **netifd:** since 22.03 it creates bridges with `RTM_NEWLINK` and `IFLA_BR_*` attributes (vlan_filtering, priority, the multicast set). Linux 4.1 defines only `IFLA_BR_FORWARD_DELAY…IFLA_BR_PRIORITY` (`include/uapi/linux/if_link.h`), and 4.1's `nla_parse` ignores attribute types above `maxtype`. So bridge creation works, but the newer tuning attributes are dropped [I from source]. `bridge-vlan` sections need `BRIDGE_VLAN_FILTERING` and do not work. `setns(CLONE_NEWNET)` is used only for netns/veth features.
- **fstools:** overlay is mounted as `lowerdir=/,upperdir=…/upper,workdir=…/work` (upstream overlayfs ≥ 3.18). UBIFS, JFFS2 and ext4 overlays are supported, f2fs is not.
- **firewall3:** `set_rule_tag()` adds `-m comment --comment "!fw3…"` to **every** rule, and flushes find fw3's own rules by that comment (`has_rule_tag()`). Without `xt_comment` the kernel rejects every rule, so **`xt_comment.ko` is mandatory** (built in §7b). Tables that fail to open are skipped (`fw3_ipt_open` → `continue`), so the missing IPv4 raw table is tolerated. Keep `flow_offloading` off: the `FLOWOFFLOAD` target is an OpenWrt kernel patch. The `firewall` package `DEPENDS` on `kmod-ipt-core/conntrack/nat`. Satisfy them with stub packages or a forced install, since the real modules come from the stock set [I].
- **firewall4:** `ruleset.uc` declares `table inet fw4` with `type nat hook prerouting/postrouting` chains. nftables wiki: "Since Linux kernel 5.2, there is support for performing stateful NAT in *inet* family chains" ([wiki.nftables.org](https://wiki.nftables.org/wiki-nftables/index.php/Performing_Network_Address_Translation_(NAT))). On top of that, `NF_TABLES` is not set. **fw4 cannot work on this kernel** [V].

**Practical recipe for OpenWrt 24.10 userspace on the stock kernel [I, untested]:**
1. Build an aarch64_cortex-a53 rootfs with the 24.10 ImageBuilder for `bcm4908/generic`. Drop `kernel`, `kmod-*`, `firewall4` and `nftables`; add `firewall`, `iptables-zz-legacy` and `ip6tables-zz-legacy`.
2. Copy in the stock `/lib/modules/4.1.52-5.02L.07/` and `/usr/lib/modules/4.1.52-5.02L.07/`, plus the rebuilt `xt_comment.ko` and any other modules you need (§7c). Write `/etc/modules.d` files for phase 1 of §2c, and add an init script that runs the S11 steps your profile needs (§2d), including `/proc/nvram` calibration for WiFi.
3. Boot path, flash layout and the stock `init=/etc/preinit`/UBI volume names versus what fstools expects (`rootfs_data`) are covered in doc 01. Check them before flashing [U].
4. Put the 32-bit glibc 2.27 libraries in a side directory for `bdmf_shell`, `wl`, `dhd`, `ethctl`, `fc` and `serdesctrl` (§5).
5. WiFi: see 06 §3–4.

---

## Cross-references
- Module load / RDPA / license blob & bootloader bank detail → boot/flash + Ethernet/Runner agents.
- PON stack (bcmgpon/gponstack/pmd) → PON agent. MCU/screen/buttons/USB → peripherals agent.
- WLAN specifics (dhd, firmware, radios) → **06-wifi.md**.
- Kernel/flash boot path for an OpenWrt rootfs on the stock kernel → **01**. Ethernet minimum module set → **02 §9**. Mainline driver status → **10**.

---

## Appendix A. Per-module inventory (143 modules) [V]

Generated by `$P/work/os/scripts/OS_modinv.sh` (modinfo) and `OS_mod_appendix.py` (loader map from `etc/modules.d`, `etc/bcm-base-drivers.sh`, `etc/init.d/{qosa,register_mcu_button_reboot,usb_mod}`, and `wanconf`/hg6d plugin strings). A module without a path suffix lives in `lib/modules/4.1.52-5.02L.07/`. All 143 have vermagic `4.1.52-5.02L.07 SMP preempt mod_unload aarch64`. Repeated `GPL` tags are collapsed.

| # | Module | Licence tag | Depends | Description | Loaded by | Size (B) |
|---|---|---|---|---|---|---|
| 1 | `arptable_filter` | GPL | - | arptables filter table | S10 modules.d/49-arptables | 4856 |
| 2 | `arpt_mangle` | GPL | - | arptables arp payload mangle target | S10 modules.d/49-arptables | 3888 |
| 3 | `bcmbrfp` | GPL | bdmf, rdpa_gpl, rdpa_mw | - | S11 | 15520 |
| 4 | `bcm_enet` | GPL | bdmf, rdpa_gpl | BCM internal ethernet network driver | S11 | 169232 |
| 5 | `bcmepon` | Proprietary | rdpa_gpl, bdmf, rdpa, bcm_enet, bcm_pondrv, rdpa_gpl_ext, opticaldet, pmd | Broadcom EPON Driver | S15 wanconf (per WAN type) | 1705416 |
| 6 | `bcmgpon` | Proprietary | bdmf, rdpa_gpl, bcm_enet, bcm_pondrv, opticaldet, gponstack, rdpa, rdpa_gpl_ext | Broadcom GPON Driver | S15 wanconf (per WAN type) | 271248 |
| 7 | `bcm_ingqos` | Proprietary | - | Broadcom Ingress QoS Module | S10 modules.d/60-bcm-ingqos | 43816 |
| 8 | `bcmlibs` | Proprietary | - | Broadcom Library of Utilities (bcmlibs) | S10 modules.d/10-bcmlibs, S11 | 34752 |
| 9 | `bcm_license` | Proprietary | - | Broadcom License driver | S11 | 30440 |
| 10 | `bcmmcast` | GPL | - | - | S11 | 124800 |
| 11 | `bcm_pondrv` | Proprietary | opticaldet | - | S11 | 404336 |
| 12 | `bcm_spdsvc` | Proprietary | bdmf, rdpa_gpl | Broadcom Speed Service Driver (spdsvc) | S11 | 30784 |
| 13 | `bcm_tcpspdtest` | Proprietary | rdpa_gpl, bdmf | Tcp Termination driver for HTTP/FTP/IPERF3 Speedtest | S11 | 121344 |
| 14 | `bcm_usb` | GPL | - | - | usb_mod (via hg6d libusb-smu) | 8512 |
| 15 | `bcmvlan` | Proprietary | - | - | S11 | 166256 |
| 16 | `bdmf` | GPL | - | BDMF shell module | S11 | 312864 |
| 17 | `bsp_gpio_data` | Proprietary | bsp_gpio_lkm | Contains all board data, unloaded after use | S10 modules.d/33-hal-bsp | 38160 |
| 18 | `bsp_gpio_lkm` | GPL | - | Bsp Sagem Module | S10 modules.d/33-hal-bsp | 83712 |
| 19 | `cfg80211` | GPL | - | wireless configuration support | S10 modules.d/20-cfg80211-nodeps, S11 | 314312 |
| 20 | `chipinfo` | proprietary | - | - | S11 | 4528 |
| 21 | `cifs-server` | GPL | - | Linux kernel CIFS/SMB SERVER | S10 modules.d/35-cifs-server, hg6d libstorage-smu | 398216 |
| 22 | `cls_u32` | GPL | - | - | S10 modules.d/99-sched | 14000 |
| 23 | `cmdlist` | Proprietary | - | Broadcom Command List Driver | S11 | 129592 |
| 24 | `dhd` | GPL and additional rights | bdmf, hnd, rdpa_gpl, cfg80211, wlcsm, bcmlibs, bcmmcast, wfd, emf, igs | - | S11 | 1349232 |
| 25 | `drv_gpio` | Dual BSD/GPL | - | LED and BUTTON driver | S10 modules.d/34-hal-gpio | 21184 |
| 26 | `dsphal` | GPL | - | BRCM VoIP DSPHAL driver | S10 modules.d/93-broadcom-voicedriver-5.02L.07 | 86632 |
| 27 | `ehci-hcd` | GPL | - | USB 2.0 'Enhanced' Host Controller (EHCI) Driver | usb_mod (via hg6d libusb-smu) | 53920 |
| 28 | `ehci-pci` | GPL | ehci-hcd | EHCI PCI platform driver | not loaded by any init script | 9736 |
| 29 | `ehci-platform` | GPL | ehci-hcd | EHCI generic platform driver | usb_mod (via hg6d libusb-smu) | 11552 |
| 30 | `emf` | Proprietary | hnd, bcmmcast | - | S11 | 27816 |
| 31 | `fusb301` | GPL v2 | - | I2C bus driver for FUSB301 USB Type-C | usb_mod (via hg6d libusb-smu) | 14872 |
| 32 | `gponstack` | Proprietary | bcm_pondrv, bdmf, pmd, opticaldet, rdpa_gpl | Broadcom GPON Stack | S15 wanconf (GPON; string is a tail-merged suffix of ngponstack.ko) [I] | 686104 |
| 33 | `gre` | GPL | - | GRE over IPv4 demultiplexer driver | S11 | 9504 |
| 34 | `hard_watchdog` | Sagem Communications | - | Hard Watchdog Driver | S10 modules.d/33-watchdog | 11464 |
| 35 | `hnd` | GPL and additional rights | wlcsm | - | S11 | 490512 |
| 36 | `igs` | Proprietary | emf, hnd, bcmmcast | - | S11 | 39520 |
| 37 | `ip6table_filter` | GPL | ip6_tables | ip6tables filter table | S10 modules.d/49-ip6tables | 5448 |
| 38 | `ip6table_mangle` | GPL | ip6_tables | ip6tables mangle table | S10 modules.d/49-ip6tables | 5216 |
| 39 | `ip6table_raw` | GPL | ip6_tables | - | S10 modules.d/49-ip6tables | 4784 |
| 40 | `ip6_tables` | GPL | - | IPv6 packet filter | S10 modules.d/49-ip6tables | 26800 |
| 41 | `ip6t_REJECT` | GPL | nf_reject_ipv6 | Xtables: packet "rejection" target for IPv6 | S10 modules.d/49-ip6tables | 5024 |
| 42 | `ip_gre` | GPL | gre | - | S11 | 19448 |
| 43 | `iptable_filter` | GPL | - | iptables filter table | S10 modules.d/40-ipt-core | 5472 |
| 44 | `iptable_mangle` | GPL | - | iptables mangle table | S10 modules.d/40-ipt-core | 5168 |
| 45 | `iptable_nat` | GPL | nf_nat_ipv4 | - | S10 modules.d/42-ipt-nat | 5752 |
| 46 | `ipt_MASQUERADE` | GPL | nf_nat_masquerade_ipv4 | Xtables: automatic-address SNAT | S10 modules.d/42-ipt-nat | 3848 |
| 47 | `ipt_REJECT` | GPL | nf_reject_ipv4 | Xtables: packet "rejection" target for IPv4 | S10 modules.d/40-ipt-core | 4568 |
| 48 | `libcrc32c` | GPL | - | CRC32c (Castagnoli) calculations | S10 modules.d/20-libcrc32c | 4344 |
| 49 | `m02099` | GPL | opticaldet | M02099 DRIVER | S20 qosa | 26648 |
| 50 | `m02180` | GPL | opticaldet | M02180 DRIVER | S20 qosa | 19176 |
| 51 | `mcu-core` | GPL | - | mcu mfd core | S29 | 11088 |
| 52 | `mcu-i2c` | GPL | mcu-core | I2C driver for mcu | S29 | 4696 |
| 53 | `mcu-keys` | GPL | - | Key driver for mcu | S29 | 11160 |
| 54 | `nf_conntrack_ftp` | GPL | nf_conntrack | ftp connection tracking helper | S10 modules.d/45-ipt-nathelper | 13592 |
| 55 | `nf_conntrack_h323` | GPL | nf_conntrack | H.323 connection tracking helper | S10 modules.d/45-ipt-nathelper-extra | 54216 |
| 56 | `nf_conntrack_ipv4` | GPL | nf_conntrack, nf_defrag_ipv4 | - | S10 modules.d/30-ipt-conntrack | 28648 |
| 57 | `nf_conntrack_ipv6` | GPL | nf_conntrack, nf_defrag_ipv6 | - | S10 modules.d/49-ip6tables | 17568 |
| 58 | `nf_conntrack_irc` | GPL | nf_conntrack | IRC (DCC) connection tracking helper | S10 modules.d/45-ipt-nathelper | 11008 |
| 59 | `nf_conntrack` | GPL | - | - | S10 modules.d/30-ipt-conntrack | 139592 |
| 60 | `nf_conntrack_netlink` | GPL | nf_conntrack | - | S10 modules.d/49-nf-conntrack-netlink | 42512 |
| 61 | `nf_conntrack_pptp` | GPL | nf_conntrack, nf_conntrack_proto_gre | Netfilter connection tracking helper module for PPTP | S10 modules.d/45-ipt-nathelper-extra | 10160 |
| 62 | `nf_conntrack_proto_gre` | GPL | nf_conntrack | - | S10 modules.d/45-ipt-nathelper-extra | 10032 |
| 63 | `nf_conntrack_rtsp` | GPL | nf_conntrack | RTSP connection tracking helper | S10 modules.d/45-ipt-nathelper-extra, S10 modules.d/99-linux-rtsp | 16080 |
| 64 | `nf_conntrack_sip` | GPL | nf_conntrack | SIP connection tracking helper | S10 modules.d/45-ipt-nathelper-extra | 42952 |
| 65 | `nf_conntrack_tftp` | GPL | nf_conntrack | TFTP connection tracking helper | S10 modules.d/45-ipt-nathelper | 8992 |
| 66 | `nf_defrag_ipv4` | GPL | - | - | S10 modules.d/30-ipt-conntrack | 4840 |
| 67 | `nf_defrag_ipv6` | GPL | - | - | S10 modules.d/49-ip6tables | 13760 |
| 68 | `nf_log_common` | GPL | - | - | S10 modules.d/40-ipt-core | 8048 |
| 69 | `nf_log_ipv4` | GPL | nf_log_common | Netfilter IPv4 packet logging | S10 modules.d/40-ipt-core | 10424 |
| 70 | `nf_log_ipv6` | GPL | nf_log_common | Netfilter IPv6 packet logging | S10 modules.d/49-ip6tables | 10776 |
| 71 | `nf_nat_ftp` | GPL | nf_conntrack_ftp, nf_nat, nf_conntrack | ftp NAT helper | S10 modules.d/45-ipt-nathelper | 6024 |
| 72 | `nf_nat_h323` | GPL | nf_conntrack, nf_conntrack_h323, nf_nat | H.323 NAT helper | S10 modules.d/45-ipt-nathelper-extra | 14008 |
| 73 | `nf_nat_ipv4` | GPL | nf_nat, nf_conntrack | - | S10 modules.d/42-ipt-nat | 9744 |
| 74 | `nf_nat_ipv6` | GPL | nf_nat, nf_conntrack | - | S10 modules.d/42-ipt-nat, S11 | 10184 |
| 75 | `nf_nat_irc` | GPL | nf_conntrack_irc, nf_nat, nf_conntrack | IRC (DCC) NAT helper | S10 modules.d/45-ipt-nathelper | 5528 |
| 76 | `nf_nat` | GPL | nf_conntrack | - | S10 modules.d/42-ipt-nat | 26552 |
| 77 | `nf_nat_masquerade_ipv4` | GPL | nf_conntrack, nf_nat | - | S10 modules.d/42-ipt-nat | 8536 |
| 78 | `nf_nat_pptp` | GPL | nf_conntrack_pptp, nf_nat_proto_gre, nf_nat, nf_conntrack | Netfilter NAT helper module for PPTP | S10 modules.d/45-ipt-nathelper-extra | 6704 |
| 79 | `nf_nat_proto_gre` | GPL | nf_nat | Netfilter NAT protocol helper module for GRE | S10 modules.d/45-ipt-nathelper-extra | 4392 |
| 80 | `nf_nat_redirect` | GPL | nf_nat | - | S10 modules.d/42-ipt-nat | 3584 |
| 81 | `nf_nat_rtsp` | GPL | nf_conntrack_rtsp, nf_nat, nf_conntrack | RTSP NAT helper | S10 modules.d/45-ipt-nathelper-extra, S10 modules.d/99-linux-rtsp | 9832 |
| 82 | `nf_nat_sip` | GPL | nf_conntrack, nf_conntrack_sip, nf_nat | SIP NAT helper | S10 modules.d/45-ipt-nathelper-extra | 15800 |
| 83 | `nf_nat_tftp` | GPL | nf_conntrack_tftp, nf_nat, nf_conntrack | TFTP NAT helper | S10 modules.d/45-ipt-nathelper | 4120 |
| 84 | `nfnetlink_queue` | GPL | - | netfilter packet queue handler | S10 modules.d/48-nfnetlink-queue | 21792 |
| 85 | `nf_reject_ipv4` | GPL | - | - | S10 modules.d/40-ipt-core | 5472 |
| 86 | `nf_reject_ipv6` | GPL | - | - | S10 modules.d/49-ip6tables | 5920 |
| 87 | `ngponstack` | Proprietary | bcm_pondrv, bdmf, pmd, opticaldet, rdpa_gpl | Broadcom NGPON Stack | S15 wanconf (per WAN type) | 838632 |
| 88 | `ohci-hcd` | GPL | - | USB 1.1 'Open' Host Controller (OHCI) Driver | usb_mod (via hg6d libusb-smu) | 48472 |
| 89 | `ohci-pci` | GPL | ohci-hcd | OHCI PCI platform driver | not loaded by any init script | 7672 |
| 90 | `ohci-platform` | GPL | ohci-hcd | OHCI generic platform driver | usb_mod (via hg6d libusb-smu) | 11024 |
| 91 | `opticaldet` | GPL | - | Optical device detection driver | S11 | 31160 |
| 92 | `pktflow` | Proprietary | bcmlibs | Broadcom Packet Flow Cache | S11 | 312680 |
| 93 | `pktrunner` | Proprietary | rdpa_gpl, rdpa_mw, bdmf, bcmlibs, pktflow | Broadcom Runner Blog Driver | S11 | 78840 |
| 94 | `pmd` | Proprietary | bcm_pondrv, opticaldet | Pmd Device driver | S15 wanconf (per WAN type) | 93464 |
| 95 | `pwrmngtd` | Proprietary | - | - | S11 | 7736 |
| 96 | `rdpa_cmd` | GPL | bdmf, rdpa_mw, rdpa_gpl, bcm_ingqos, rdpa_gpl_ext | - | S11 | 172528 |
| 97 | `rdpa_gpl_ext` | GPL | - | - | S11 | 4104 |
| 98 | `rdpa_gpl` | GPL | bdmf | - | S11 | 61000 |
| 99 | `rdpa` | Proprietary | bdmf, rdpa_gpl, bcmlibs, bcm_pondrv | Runner Data Path API. (C) Broadcom | S11 | 5069264 |
| 100 | `rdpa_mw` | GPL | bdmf, rdpa_gpl, rdpa | RDPA MW layer | S11 | 59800 |
| 101 | `rdpa_usr` | GPL | bdmf, rdpa_gpl | rdpa_user_ag | S11 | 94856 |
| 102 | `sch_hfsc` | GPL | - | - | S10 modules.d/99-sched | 20168 |
| 103 | `sch_mqprio` | GPL | - | - | not loaded by any init script | 8016 |
| 104 | `sch_multiq` | GPL | - | - | S10 modules.d/99-sched | 9424 |
| 105 | `sch_qfq` | GPL | - | - | not loaded by any init script | 18984 |
| 106 | `sch_red` | GPL | - | - | S10 modules.d/99-sched | 10960 |
| 107 | `sch_sfb` | GPL | - | Stochastic Fair Blue queue discipline | not loaded by any init script | 10784 |
| 108 | `sch_sfq` | GPL | - | - | S10 modules.d/99-sched | 14824 |
| 109 | `sch_teql` | GPL | - | - | S10 modules.d/99-sched | 11168 |
| 110 | `sgcnet` | SAGEMCOM | - | Network driver | S10 modules.d/88-hal-network-module | 16520 |
| 111 | `slicslac` | Proprietary | - | BRCM VoIP SLIC/SLAC driver | S10 modules.d/93-broadcom-voicedriver-5.02L.07 | 808008 |
| 112 | `ssd1322` | GPL | - | Solomon SSD1322 framebuffer device (for bcm63xx) | S10 modules.d/91-ssd1322-driver-bcm63xx | 14248 |
| 113 | `uas` | GPL | usb-storage | - | S10 modules.d/70-usb-storage-uas, usb_mod (via hg6d libusb-smu) | 25912 |
| 114 | `uhci-hcd` | GPL | - | USB Universal Host Controller Interface driver | usb_mod (via hg6d libusb-smu) | 33712 |
| 115 | `usblp` | GPL | - | USB Printer Device Class driver | S10 modules.d/60-usb-printer | 21392 |
| 116 | `usb-storage` | GPL | - | USB Mass Storage driver for Linux | S10 modules.d/60-usb-storage | 109776 |
| 117 | `wfd` | GPL | bdmf, rdpa_gpl, bcmlibs, bcmmcast | WLAN Forwarding Driver | S11 | 41904 |
| 118 | `wlcsm` | Proprietary | - | - | S11 | 14616 |
| 119 | `xhci-hcd` | GPL | - | 'eXtensible' Host Controller (xHC) Driver | usb_mod (via hg6d libusb-smu) | 145168 |
| 120 | `xhci-pci` | GPL | xhci-hcd | xHCI PCI Host Controller Driver | not loaded by any init script | 8288 |
| 121 | `xhci-plat-hcd` | GPL | xhci-hcd | xHCI Platform Host Controller Driver | usb_mod (via hg6d libusb-smu) | 9488 |
| 122 | `xt_addrtype` | GPL | - | Xtables: address type match | S10 modules.d/45-ipt-extra | 6608 |
| 123 | `xt_blog` | GPL | - | iptables blog match | S10 modules.d/44-bcm-blog | 3248 |
| 124 | `xt_conntrack` | GPL | nf_conntrack | Xtables: connection tracking state match | S10 modules.d/42-ipt-conntrack-extra | 5952 |
| 125 | `xt_dscp` | GPL | - | Xtables: DSCP/TOS field match | S10 modules.d/45-ipt-ipopt | 4464 |
| 126 | `xt_DSCP` | GPL | - | Xtables: DSCP/TOS field modification | S10 modules.d/45-ipt-ipopt | 5224 |
| 127 | `xt_limit` | GPL | - | Xtables: rate-limit match | S10 modules.d/40-ipt-core | 5200 |
| 128 | `xt_LOG` | GPL | - | Xtables: IPv4/IPv6 packet logging | S10 modules.d/40-ipt-core | 4288 |
| 129 | `xt_mac` | GPL | - | Xtables: MAC address match | S10 modules.d/40-ipt-core | 3352 |
| 130 | `xt_mark` | GPL | - | Xtables: packet mark operations | S10 modules.d/45-ipt-ipopt | 3760 |
| 131 | `xt_multiport` | GPL | - | Xtables: multiple port matching for TCP, UDP, UDP-Lite, SCTP | S10 modules.d/40-ipt-core | 4240 |
| 132 | `xt_nat` | GPL | nf_nat | - | S10 modules.d/42-ipt-nat | 4704 |
| 133 | `xt_NFQUEUE` | GPL | - | Xtables: packet forwarding to netlink | S10 modules.d/48-nfnetlink-queue | 5864 |
| 134 | `xt_REDIRECT` | GPL | nf_nat_redirect | Xtables: Connection redirection to localhost | S10 modules.d/42-ipt-nat, S10 modules.d/43-ipt-nat-extra | 4000 |
| 135 | `xt_SKIPLOG` | GPL | - | iptables stop logging module | S10 modules.d/59-bcm-skiplog | 3320 |
| 136 | `xt_state` | GPL | nf_conntrack | ip[6]_tables connection tracking state match module | S10 modules.d/30-ipt-conntrack | 4000 |
| 137 | `xt_TCPMSS` | GPL | - | Xtables: TCP Maximum Segment Size (MSS) adjustment | S10 modules.d/40-ipt-core | 7264 |
| 138 | `openvswitch (usr/lib/modules/4.1.52-5.02L.07/openvswitch)` | GPL | nf_conntrack, nf_nat, nf_defrag_ipv6, libcrc32c, nf_nat_ipv6, nf_nat_ipv4, gre, nf_defrag_ipv4 | Open vSwitch switching datapath | S11 | 314296 |
| 139 | `vport-geneve (usr/lib/modules/4.1.52-5.02L.07/openvswitch)` | GPL | openvswitch | OVS: Geneve switching port | not loaded by any init script | 4552 |
| 140 | `vport-gre (usr/lib/modules/4.1.52-5.02L.07/openvswitch)` | GPL | openvswitch | OVS: GRE switching port | S11 | 4272 |
| 141 | `vport-lisp (usr/lib/modules/4.1.52-5.02L.07/openvswitch)` | GPL | openvswitch | OVS: Lisp switching port | not loaded by any init script | 4552 |
| 142 | `vport-stt (usr/lib/modules/4.1.52-5.02L.07/openvswitch)` | GPL | openvswitch | OVS: STT switching port | not loaded by any init script | 4544 |
| 143 | `vport-vxlan (usr/lib/modules/4.1.52-5.02L.07/openvswitch)` | GPL | openvswitch | OVS: VXLAN switching port | not loaded by any init script | 5704 |

---

## Revision log (pass 2, 2026-11-04)

Scope of pass 2: kernel, drivers, init, OpenWrt compatibility. **§3, §4 and §6 (middleware, accounts/credentials/remote access, persistence) were not edited.**

| # | Section | Previous claim | Pass-2 result | Evidence |
|---|---|---|---|---|
| 1 | §1 module table | "~105 GPL / ~24 proprietary"; "6 in kernel/drivers/ata\|usb subdirs plus openvswitch" | **Corrected [V]:** 116 GPL-compatible / 23 proprietary / 2 GPL+additional rights / 2 Sagemcom. 137 flat modules + 6 in `usr/lib/modules/…/openvswitch/`; no `kernel/drivers` subdirs | `modinv.tsv`, §1a, App. A |
| 2 | §1 vermagic | stated for dhd.ko | **Confirmed for all 143 [V]**, reproduced from the Merlin tree | §7b |
| 3 | §1 config highlights | brief list | **Expanded [V]:** COMPAT, namespaces, cgroups, seccomp, nf_tables, xt_comment, raw/NAT66, overlay, UBIFS, squashfs-xz, SHMEM/tmpfs, syscalls | §1b |
| 4 | §1 memory | dhd0-2 = "per-radio host DMA/descriptor pools" | **Refined [V]:** Runner DHD-offload flow-ring pools (`dhd_runner_flowmgr_init … size<11534336>`) | kernel.log |
| 5 | §2b S15 | "WAN/PON driver + laser_dev insmod" | **Corrected [V]:** wanconf loads pmd/(n)gponstack/bcmgpon/bcmepon; laser_dev and m02099/m02180 come from **S20 qosa**, which is not QoS | init scripts, `strings wanconf` |
| 6 | §2b S39 | "USB stack" | **Corrected [V]:** S39 only mounts usbfs; USB modules come from `usb_mod`, run by hg6d `libusb-smu.so` | `etc/init.d/usb*`, plugin strings |
| 7 | §2b/§2c | "S11 loads ALL kernel modules" | **Corrected [V]:** S10 `boot` loads `/etc/modules.d/*` first (netfilter, BSP GPIO, watchdog, drv_gpio, ssd1322, voice, bcm_ingqos, cfg80211, bcmlibs); then S11, S15, S20, S29 and hg6d at runtime | §2c |
| 8 | §2c | insmod chain | **Expanded [V]:** full S11 step table with parameters and E/W/P/S marks; `rdpa_cmd` depends on phase-1 `bcm_ingqos`; `cfg80211`/`bcmlibs` S11 insmods are duplicates | §2d |
| 9 | §5 | "aarch64 little-endian" userspace | **Corrected [V]:** userspace is **32-bit ARMv7 EABI5 soft-float**, glibc 2.27 (698 ARM32 ELFs; only unused `ld-linux-aarch64` copies are 64-bit). The kernel is arm64 with `COMPAT=y` | `file`, `readelf -A` |
| 10 | new §7 | – | Kernel rebuild: no bootable 6858 kernel from Merlin (BSP only as 4908 prebuilt `.o`). Module ABI verified for sk_buff, net_device, task_struct, module and nf_conn (after removing the ASUS `biqos_cb_t`); `xt_comment.ko` and `wireguard.ko` built | §7 |
| 11 | new §8 | – | OpenWrt 24.10 userspace (25.12 probably) usable with fw3; fw4 impossible (nf_tables + inet NAT ≥ 5.2) | §8 |
| 12 | §5 OpenWrt heritage | "~2016 Chaos Calmer" (untagged) | Tagged **[I]**; no version file found | – |
| 13 | §2a preinit, §2b other rows | – | Re-checked against `lib/preinit/*` and `etc/rc.d`; unchanged [V] | – |

Open items: whether `wlcsm`, `chipinfo`, `bcm_license`, `pwrmngtd` or `bsp_gpio_lkm` are needed for Ethernet only [U]; whether rebuilt modules load on hardware [U]; netifd bridge handling on 4.1 at runtime [I]; whether the hardware watchdog is armed before `hard_watchdog` loads [U].
